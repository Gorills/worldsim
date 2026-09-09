#include "worldsim/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

double base_terrain_elevation_m(
    std::uint64_t seed,
    Vec3d p,
    const TectonicSample& tectonic
) {
    // Tectonics owns the broad hypsometry. The remaining procedural terms are
    // deliberately bounded meso/local relief so they can texture the macro
    // shape without replacing it with an unrelated continent generator.
    const double rolling=220.0*fbm_unit(seed,200,p,180'000.0,5);
    const double local_detail=18.0*fbm_unit(seed,600,p,900.0,4);
    const double ridge_source=fbm_unit(seed,300,p,520'000.0,5);
    const double ridge=std::clamp(
        (1.0-std::abs(ridge_source)-0.35)/0.65,
        0.0,
        1.0
    );
    const double orogenic_detail=
        1'000.0*ridge*ridge*ridge*
        tectonic.uplift_forcing*
        (0.35+0.65*tectonic.continental_affinity);

    return std::clamp(
        tectonic.macro_elevation_m+rolling+local_detail+orogenic_detail,
        -11'000.0,
        9'000.0
    );
}

double visual_orographic_relief_m(
    std::uint64_t seed,
    Vec3d p,
    const TectonicSample& tectonic,
    double base_elevation_m
) {
    const double uplift=std::clamp(tectonic.uplift_forcing,0.0,1.0);
    if (!(uplift>0.0) || !(base_elevation_m>0.0)) return 0.0;

    // The authoritative adaptive cover stores regional mean elevation. Walking
    // still needs mountain-scale peaks below that cell size. Use sphere-native
    // ridged noise so the added relief stays deterministic and seam-free, and
    // gate it to convergent dry-land belts instead of adding generic roughness
    // everywhere.
    const double ridge_source=fbm_unit(
        seed,
        fnv1a64("terrain.visual.orography.ridge"),
        p,
        60'000.0,
        5
    );
    const double ridge=std::clamp(
        (1.0-std::abs(ridge_source)-0.15)/0.85,
        0.0,
        1.0
    );
    const double peak_noise=
        0.5+0.5*fbm_unit(
            seed,
            fnv1a64("terrain.visual.orography.peaks"),
            p,
            28'000.0,
            4
        );
    const double local_peak_noise=
        0.5+0.5*fbm_unit(
            seed,
            fnv1a64("terrain.visual.orography.ridges.local"),
            p,
            16'000.0,
            4
        );
    const double summit_profile=std::pow(
        smoothstep(0.50,0.78,local_peak_noise),
        2.0
    );
    const double land_gate=smoothstep(0.0,750.0,base_elevation_m);
    const double mountain_strength=
        std::pow(uplift,0.35)*
        (0.35+0.65*std::clamp(tectonic.continental_affinity,0.0,1.0));
    const double broad_relief=
        ridge*ridge*(0.55+0.45*peak_noise);
    const double summit_relief=
        summit_profile*(0.35+0.65*ridge);

    // Restore kilometre-scale ribs and gullies with one single-octave value
    // noise lookup instead of the rejected multi-FBM crag/gully/spur stack.
    // The expensive broad 60/28/16 km fields above already place the mountain;
    // this term only breaks smooth uplifted faces into local alpine structure.
    const double detail_frequency=kEarthRadiusM/6'500.0;
    const double detail_source=value_noise3(
        seed,
        fnv1a64("terrain.visual.orography.detail"),
        p.x*detail_frequency,
        p.y*detail_frequency,
        p.z*detail_frequency
    );
    const double detail_ridge=std::pow(
        std::clamp(
            (1.0-std::abs(detail_source)-0.08)/0.92,
            0.0,
            1.0
        ),
        4.0
    );
    const double alpine_gate=smoothstep(
        0.12,
        0.58,
        std::max(broad_relief,summit_relief)
    );
    const double local_relief=
        520.0*
        land_gate*
        mountain_strength*
        alpine_gate*
        (1.25*detail_ridge-0.38+0.18*detail_source);

    return
        land_gate*
        mountain_strength*
        (
            1'800.0*broad_relief+
            2'050.0*summit_relief
        )+
        local_relief;
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

TerrainSample TerrainGenerator::sample_visual_projected(
    double east_m,
    double north_m
) const {
    if (!std::isfinite(east_m) || !std::isfinite(north_m))
        return {-5'000.0,0.0};
    return sample_visual_direction(projected_to_direction(east_m,north_m));
}

TerrainSample TerrainGenerator::sample_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);
    const TectonicSample tectonic=tectonics_.sample_direction(p);
    const double elevation=base_terrain_elevation_m(seed_,p,tectonic);
    const double land_fraction=smoothstep(-75.0,75.0,elevation);
    return {elevation,land_fraction};
}

TerrainSample TerrainGenerator::sample_visual_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);
    const TectonicSample tectonic=tectonics_.sample_direction(p);
    const double base_elevation=base_terrain_elevation_m(seed_,p,tectonic);
    const double elevation=std::clamp(
        base_elevation+
        visual_orographic_relief_m(seed_,p,tectonic,base_elevation),
        -11'000.0,
        9'000.0
    );
    const double land_fraction=smoothstep(-75.0,75.0,elevation);
    return {elevation,land_fraction};
}

} // namespace worldsim
