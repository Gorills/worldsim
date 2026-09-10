#include "worldsim/spatial.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>

namespace worldsim {

namespace {
constexpr std::uint64_t kFaceShift=61U;
constexpr std::uint64_t kLevelShift=56U;
constexpr std::uint64_t kCoordMask=(1ULL<<28U)-1ULL;
constexpr std::uint64_t kXShift=28U;
constexpr double kBoundaryPlaneTolerance=1.0e-10;

double unwrap_near(double angle, double reference) {
    while (angle-reference>kPi) angle-=2.0*kPi;
    while (angle-reference<-kPi) angle+=2.0*kPi;
    return angle;
}

double overlapping_arc_radians(
    Vec3d source_a,
    Vec3d source_b,
    Vec3d candidate_a,
    Vec3d candidate_b
) {
    const Vec3d source_normal_raw=cross(source_a,source_b);
    const double source_normal_length=norm(source_normal_raw);
    if (!(source_normal_length>0.0)) return 0.0;
    const Vec3d source_normal=
        source_normal_raw*(1.0/source_normal_length);
    if (
        std::abs(dot(source_normal,candidate_a))>
            kBoundaryPlaneTolerance ||
        std::abs(dot(source_normal,candidate_b))>
            kBoundaryPlaneTolerance
    ) {
        return 0.0;
    }

    const Vec3d tangent=normalized(cross(source_normal,source_a));
    double source_length=std::atan2(
        dot(source_b,tangent),
        dot(source_b,source_a)
    );
    if (source_length<0.0) source_length+=2.0*kPi;
    if (!(source_length>0.0) || source_length>=kPi)
        return 0.0;

    const double midpoint=0.5*source_length;
    auto coordinate=[&](Vec3d point) {
        return unwrap_near(
            std::atan2(
                dot(point,tangent),
                dot(point,source_a)
            ),
            midpoint
        );
    };
    double candidate_start=coordinate(candidate_a);
    double candidate_end=coordinate(candidate_b);
    while (candidate_end-candidate_start>kPi)
        candidate_end-=2.0*kPi;
    while (candidate_start-candidate_end>kPi)
        candidate_end+=2.0*kPi;

    const double candidate_low=
        std::min(candidate_start,candidate_end);
    const double candidate_high=
        std::max(candidate_start,candidate_end);
    return std::max(
        0.0,
        std::min(source_length,candidate_high)-
            std::max(0.0,candidate_low)
    );
}
}

CellId CellId::make(std::uint8_t face, std::uint8_t level, std::uint32_t x, std::uint32_t y) {
    if (face>=6 || level>kMaxLevel) throw std::invalid_argument("invalid CellId face/level");
    const std::uint32_t n=1U<<level;
    if (x>=n || y>=n) throw std::invalid_argument("invalid CellId coordinate");
    return CellId{(static_cast<std::uint64_t>(face)<<kFaceShift) |
                  (static_cast<std::uint64_t>(level)<<kLevelShift) |
                  (static_cast<std::uint64_t>(x)<<kXShift) |
                  static_cast<std::uint64_t>(y)};
}

std::uint8_t CellId::face() const { return static_cast<std::uint8_t>((raw_>>kFaceShift)&0x7ULL); }
std::uint8_t CellId::level() const { return static_cast<std::uint8_t>((raw_>>kLevelShift)&0x1FULL); }
std::uint32_t CellId::x() const { return static_cast<std::uint32_t>((raw_>>kXShift)&kCoordMask); }
std::uint32_t CellId::y() const { return static_cast<std::uint32_t>(raw_&kCoordMask); }

bool CellId::valid() const {
    if (face()>=6 || level()>kMaxLevel) return false;
    const std::uint32_t n=1U<<level();
    return x()<n && y()<n;
}

CellId CellId::parent() const {
    if (level()==0) return *this;
    return make(face(),static_cast<std::uint8_t>(level()-1U),x()>>1U,y()>>1U);
}

std::array<CellId,4> CellId::children() const {
    if (level()>=kMaxLevel) throw std::out_of_range("CellId max level reached");
    const auto l=static_cast<std::uint8_t>(level()+1U);
    const auto xx=x()<<1U;
    const auto yy=y()<<1U;
    return {make(face(),l,xx,yy),make(face(),l,xx+1U,yy),make(face(),l,xx,yy+1U),make(face(),l,xx+1U,yy+1U)};
}

Vec3d CubeSphereTopology::face_uv_to_xyz(std::uint8_t face, double u, double v) {
    switch (face) {
        case 0: return { 1.0,  u,   v};   // +X
        case 1: return {-1.0, -u,   v};   // -X
        case 2: return {-u,   1.0,  v};   // +Y
        case 3: return { u,  -1.0,  v};   // -Y
        case 4: return {-v,   u,   1.0};   // +Z
        case 5: return { v,   u,  -1.0};   // -Z
        default: throw std::invalid_argument("invalid cube face");
    }
}

std::tuple<std::uint8_t,double,double> CubeSphereTopology::xyz_to_face_uv(Vec3d p) {
    const double scale=std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)});
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !(scale>0.0))
        throw std::invalid_argument("direction must be finite and non-zero");
    // Face ratios are scale-invariant. Divide components directly to support
    // both subnormal and very large finite directions without norm overflow.
    p={p.x/scale,p.y/scale,p.z/scale};
    const double ax=std::abs(p.x), ay=std::abs(p.y), az=std::abs(p.z);
    if (ax>=ay && ax>=az) {
        if (p.x>=0.0) return {0,p.y/p.x,p.z/p.x};
        return {1,p.y/p.x,-p.z/p.x};
    }
    if (ay>=ax && ay>=az) {
        if (p.y>=0.0) return {2,-p.x/p.y,p.z/p.y};
        return {3,-p.x/p.y,-p.z/p.y};
    }
    if (p.z>=0.0) return {4,p.y/p.z,-p.x/p.z};
    return {5,-p.y/p.z,-p.x/p.z};
}

Vec3d CubeSphereTopology::center_unit(CellId id) const {
    if (!id.valid()) throw std::invalid_argument("invalid cell");
    const double n=static_cast<double>(1U<<id.level());
    const double u=-1.0+(static_cast<double>(id.x())+0.5)*(2.0/n);
    const double v=-1.0+(static_cast<double>(id.y())+0.5)*(2.0/n);
    return normalized(face_uv_to_xyz(id.face(),u,v));
}

std::array<Vec3d,4> CubeSphereTopology::corners_unit(CellId id) const {
    const double n=static_cast<double>(1U<<id.level());
    const double u0=-1.0+static_cast<double>(id.x())*(2.0/n);
    const double v0=-1.0+static_cast<double>(id.y())*(2.0/n);
    const double u1=u0+2.0/n;
    const double v1=v0+2.0/n;
    return {normalized(face_uv_to_xyz(id.face(),u0,v0)),
            normalized(face_uv_to_xyz(id.face(),u1,v0)),
            normalized(face_uv_to_xyz(id.face(),u1,v1)),
            normalized(face_uv_to_xyz(id.face(),u0,v1))};
}

double CubeSphereTopology::spherical_triangle_area(Vec3d a, Vec3d b, Vec3d c) {
    const double numerator=std::abs(dot(a,cross(b,c)));
    const double denominator=1.0+dot(a,b)+dot(b,c)+dot(c,a);
    return 2.0*std::atan2(numerator,denominator);
}

double CubeSphereTopology::area_m2(CellId id) const {
    const auto c=corners_unit(id);
    const double steradians=spherical_triangle_area(c[0],c[1],c[2])+spherical_triangle_area(c[0],c[2],c[3]);
    return steradians*kEarthRadiusM*kEarthRadiusM;
}

double CubeSphereTopology::shared_boundary_length_m(
    CellId a,
    CellId b
) const {
    if (!a.valid() || !b.valid())
        throw std::invalid_argument("invalid cell");
    if (a==b) return 0.0;

    constexpr std::array<std::array<std::size_t,2>,4> edges{{
        {{0U,3U}},
        {{1U,2U}},
        {{0U,1U}},
        {{3U,2U}}
    }};
    const auto a_corners=corners_unit(a);
    const auto b_corners=corners_unit(b);
    double overlap_radians=0.0;
    for (const auto& a_edge:edges) {
        for (const auto& b_edge:edges) {
            overlap_radians+=overlapping_arc_radians(
                a_corners[a_edge[0]],
                a_corners[a_edge[1]],
                b_corners[b_edge[0]],
                b_corners[b_edge[1]]
            );
        }
    }
    return overlap_radians*kEarthRadiusM;
}

CellId CubeSphereTopology::from_direction(Vec3d direction, std::uint8_t level) const {
    if (level>CellId::kMaxLevel) throw std::invalid_argument("level too high");
    auto [face,u,v]=xyz_to_face_uv(direction);
    u=std::clamp(u,-1.0,std::nextafter(1.0,0.0));
    v=std::clamp(v,-1.0,std::nextafter(1.0,0.0));
    const std::uint32_t n=1U<<level;
    const auto fx=std::clamp(static_cast<std::uint32_t>(((u+1.0)*0.5)*static_cast<double>(n)),0U,n-1U);
    const auto fy=std::clamp(static_cast<std::uint32_t>(((v+1.0)*0.5)*static_cast<double>(n)),0U,n-1U);
    return CellId::make(face,level,fx,fy);
}

std::array<CellId,4> CubeSphereTopology::neighbors4(CellId id) const {
    const std::int64_t n=static_cast<std::int64_t>(1U<<id.level());
    const std::array<std::pair<int,int>,4> d{{{-1,0},{1,0},{0,-1},{0,1}}};
    std::array<CellId,4> out{};
    for (std::size_t i=0;i<d.size();++i) {
        const std::int64_t nx=static_cast<std::int64_t>(id.x())+d[i].first;
        const std::int64_t ny=static_cast<std::int64_t>(id.y())+d[i].second;
        if (nx>=0 && nx<n && ny>=0 && ny<n) {
            out[i]=CellId::make(id.face(),id.level(),static_cast<std::uint32_t>(nx),static_cast<std::uint32_t>(ny));
            continue;
        }
        const double u=-1.0+(static_cast<double>(nx)+0.5)*(2.0/static_cast<double>(n));
        const double v=-1.0+(static_cast<double>(ny)+0.5)*(2.0/static_cast<double>(n));
        out[i]=from_direction(face_uv_to_xyz(id.face(),u,v),id.level());
    }
    return out;
}

std::pair<double,double> CubeSphereTopology::lat_lon_rad(CellId id) const {
    const auto p=center_unit(id);
    return {std::asin(std::clamp(p.z,-1.0,1.0)),std::atan2(p.y,p.x)};
}

std::set<CellId> uniform_cover(std::uint8_t level) {
    if (level>CellId::kMaxLevel) throw std::invalid_argument("level too high");
    std::set<CellId> cells;
    const std::uint32_t n=1U<<level;
    for (std::uint8_t face=0;face<6;++face)
        for (std::uint32_t x=0;x<n;++x)
            for (std::uint32_t y=0;y<n;++y)
                cells.insert(CellId::make(face,level,x,y));
    return cells;
}

} // namespace worldsim
