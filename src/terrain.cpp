#include "worldsim/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace worldsim {
namespace {

constexpr double kCenterLatitudeRad=45.0*kPi/180.0;
constexpr double kCenterLongitudeRad=70.0*kPi/180.0;

double smoothstep(double a, double b, double x) {
    const double t=std::clamp((x-a)/(b-a),0.0,1.0);
    return t*t*(3.0-2.0*t);
}

double quintic(double t) {
    return t*t*t*(t*(t*6.0-15.0)+10.0);
}

const Vec3d& continent_center() {
    static const Vec3d value=[] {
        const double cos_lat=std::cos(kCenterLatitudeRad);
        return Vec3d{
            cos_lat*std::cos(kCenterLongitudeRad),
            cos_lat*std::sin(kCenterLongitudeRad),
            std::sin(kCenterLatitudeRad)
        };
    }();
    return value;
}

const Vec3d& continent_east() {
    static const Vec3d value{
        -std::sin(kCenterLongitudeRad),
        std::cos(kCenterLongitudeRad),
        0.0
    };
    return value;
}

const Vec3d& continent_north() {
    static const Vec3d value=normalized(cross(continent_center(),continent_east()));
    return value;
}

double lattice3(std::uint64_t seed,
                std::uint64_t stream,
                std::int64_t x,
                std::int64_t y,
                std::int64_t z) {
    const auto ux=static_cast<std::uint64_t>(x);
    const auto uy=static_cast<std::uint64_t>(y);
    const auto uz=static_cast<std::uint64_t>(z);
    const std::uint64_t h=mix64(
        seed ^
        mix64(stream) ^
        mix64(ux) ^
        mix64(uy*0xd6e8feb86659fd93ULL) ^
        mix64(uz*0xa5a3564e27f8862bULL)
    );
    return static_cast<double>(h>>11U)*(2.0/9007199254740992.0)-1.0;
}

double value_noise3(std::uint64_t seed,
                    std::uint64_t stream,
                    double x,
                    double y,
                    double z) {
    const auto x0=static_cast<std::int64_t>(std::floor(x));
    const auto y0=static_cast<std::int64_t>(std::floor(y));
    const auto z0=static_cast<std::int64_t>(std::floor(z));
    const double sx=quintic(x-static_cast<double>(x0));
    const double sy=quintic(y-static_cast<double>(y0));
    const double sz=quintic(z-static_cast<double>(z0));

    const double c000=lattice3(seed,stream,x0,y0,z0);
    const double c100=lattice3(seed,stream,x0+1,y0,z0);
    const double c010=lattice3(seed,stream,x0,y0+1,z0);
    const double c110=lattice3(seed,stream,x0+1,y0+1,z0);
    const double c001=lattice3(seed,stream,x0,y0,z0+1);
    const double c101=lattice3(seed,stream,x0+1,y0,z0+1);
    const double c011=lattice3(seed,stream,x0,y0+1,z0+1);
    const double c111=lattice3(seed,stream,x0+1,y0+1,z0+1);

    const double x00=c000+(c100-c000)*sx;
    const double x10=c010+(c110-c010)*sx;
    const double x01=c001+(c101-c001)*sx;
    const double x11=c011+(c111-c011)*sx;
    const double y0v=x00+(x10-x00)*sy;
    const double y1v=x01+(x11-x01)*sy;
    return y0v+(y1v-y0v)*sz;
}

double fbm_unit(std::uint64_t seed,
                 std::uint64_t stream,
                 Vec3d p,
                 double scale_m,
                 int octaves) {
    double amplitude=1.0;
    double frequency=kEarthRadiusM/scale_m;
    double total=0.0;
    double normalization=0.0;
    for (int octave=0;octave<octaves;++octave) {
        const auto octave_stream=stream+static_cast<std::uint64_t>(octave)*17ULL;
        total+=amplitude*value_noise3(
            seed,
            octave_stream,
            p.x*frequency,
            p.y*frequency,
            p.z*frequency
        );
        normalization+=amplitude;
        amplitude*=0.5;
        frequency*=2.0;
    }
    return total/normalization;
}

// Smooth anisotropic cap on the sphere. Near the continent center the tangent
// terms reproduce the nominal east/north semi-axes. The normal/back term keeps
// the shape closed and continuous on the far side of the planet, including the
// antipode where azimuthal map coordinates are singular.
double continent_sdf(std::uint64_t seed, Vec3d p) {
    const Vec3d center=continent_center();
    const Vec3d east=continent_east();
    const Vec3d north=continent_north();

    const double east_scaled=dot(p,east)*kEarthRadiusM/TerrainGenerator::kContinentSemiMajorM;
    const double north_scaled=dot(p,north)*kEarthRadiusM/TerrainGenerator::kContinentSemiMinorM;
    const double back=1.0-std::clamp(dot(p,center),-1.0,1.0);
    const double radial=std::sqrt(
        east_scaled*east_scaled+
        north_scaled*north_scaled+
        back*back
    );
    const double coast_noise=0.10*fbm_unit(seed,100,p,1'600'000.0,4);
    return 1.0-radial+coast_noise;
}

} // namespace

std::pair<double,double> TerrainGenerator::direction_to_projected(Vec3d direction) {
    const Vec3d p=normalized(direction);
    const double latitude=std::asin(std::clamp(p.z,-1.0,1.0));
    const double longitude=std::atan2(p.y,p.x);
    double delta_longitude=longitude-kCenterLongitudeRad;
    while (delta_longitude>kPi) delta_longitude-=2.0*kPi;
    while (delta_longitude<-kPi) delta_longitude+=2.0*kPi;

    const double sin_lat=std::sin(latitude);
    const double cos_lat=std::cos(latitude);
    const double sin_center=std::sin(kCenterLatitudeRad);
    const double cos_center=std::cos(kCenterLatitudeRad);
    const double cos_c=std::clamp(
        sin_center*sin_lat+cos_center*cos_lat*std::cos(delta_longitude),
        -1.0,
        1.0
    );
    const double c=std::acos(cos_c);
    if (c<1.0e-12) return {0.0,0.0};

    const double sin_c=std::sin(c);
    if (std::abs(sin_c)<1.0e-12)
        return {kEarthRadiusM*c,0.0};

    const double k=c/sin_c;
    const double east=kEarthRadiusM*k*cos_lat*std::sin(delta_longitude);
    const double north=kEarthRadiusM*k*(
        cos_center*sin_lat-sin_center*cos_lat*std::cos(delta_longitude)
    );
    return {east,north};
}

Vec3d TerrainGenerator::projected_to_direction(double east_m, double north_m) {
    const double rho=std::hypot(east_m,north_m);
    const double sin_center=std::sin(kCenterLatitudeRad);
    const double cos_center=std::cos(kCenterLatitudeRad);
    if (rho<1.0e-9) {
        return {
            cos_center*std::cos(kCenterLongitudeRad),
            cos_center*std::sin(kCenterLongitudeRad),
            sin_center
        };
    }

    const double c=rho/kEarthRadiusM;
    const double sin_c=std::sin(c);
    const double cos_c=std::cos(c);
    const double latitude=std::asin(std::clamp(
        cos_c*sin_center+(north_m*sin_c*cos_center/rho),
        -1.0,
        1.0
    ));
    const double longitude=kCenterLongitudeRad+std::atan2(
        east_m*sin_c,
        rho*cos_center*cos_c-north_m*sin_center*sin_c
    );
    const double cos_lat=std::cos(latitude);
    return normalized({
        cos_lat*std::cos(longitude),
        cos_lat*std::sin(longitude),
        std::sin(latitude)
    });
}

TerrainSample TerrainGenerator::sample_projected(double east_m, double north_m) const {
    if (!std::isfinite(east_m) || !std::isfinite(north_m))
        return {-5'000.0,0.0};
    return sample_direction(projected_to_direction(east_m,north_m));
}

TerrainSample TerrainGenerator::sample_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);
    const double sdf=continent_sdf(seed_,p);
    const double land_fraction=smoothstep(-0.025,0.025,sdf);
    double elevation=0.0;

    if (sdf>=0.0) {
        const double interior=std::clamp(sdf,0.0,1.0);
        const double rolling=220.0*fbm_unit(seed_,200,p,180'000.0,5);
        const double local_detail=18.0*fbm_unit(seed_,600,p,900.0,4);
        const double ridge_source=fbm_unit(seed_,300,p,520'000.0,5);
        const double ridge=std::clamp((1.0-std::abs(ridge_source)-0.35)/0.65,0.0,1.0);
        const double uplift=0.5+0.5*fbm_unit(seed_,400,p,900'000.0,3);
        const double mountain_zone=smoothstep(0.08,0.72,sdf)*smoothstep(0.0,1.0,uplift);
        const double mountains=3'800.0*ridge*ridge*ridge*mountain_zone;
        elevation=120.0+950.0*std::pow(interior,0.75)+rolling+local_detail+mountains;
    } else {
        const double deep=smoothstep(0.0,0.8,-sdf);
        const double ocean_noise=350.0*fbm_unit(seed_,500,p,900'000.0,4);
        elevation=-180.0-4'700.0*deep+ocean_noise*(0.2+0.8*deep);
    }

    return {
        std::clamp(elevation,-11'000.0,9'000.0),
        land_fraction
    };
}

} // namespace worldsim
