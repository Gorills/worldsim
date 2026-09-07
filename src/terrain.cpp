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

double lattice(std::uint64_t seed, std::uint64_t stream, std::int64_t x, std::int64_t y) {
    const auto ux=static_cast<std::uint64_t>(x);
    const auto uy=static_cast<std::uint64_t>(y);
    const std::uint64_t h=mix64(seed ^ mix64(stream) ^ mix64(ux) ^ mix64(uy*0xd6e8feb86659fd93ULL));
    return static_cast<double>(h>>11U)*(2.0/9007199254740992.0)-1.0;
}

double value_noise(std::uint64_t seed, std::uint64_t stream, double x, double y) {
    const auto x0=static_cast<std::int64_t>(std::floor(x));
    const auto y0=static_cast<std::int64_t>(std::floor(y));
    const double tx=x-static_cast<double>(x0);
    const double ty=y-static_cast<double>(y0);
    const double sx=quintic(tx);
    const double sy=quintic(ty);

    const double a=lattice(seed,stream,x0,y0);
    const double b=lattice(seed,stream,x0+1,y0);
    const double c=lattice(seed,stream,x0,y0+1);
    const double d=lattice(seed,stream,x0+1,y0+1);
    const double ab=a+(b-a)*sx;
    const double cd=c+(d-c)*sx;
    return ab+(cd-ab)*sy;
}

double fbm(std::uint64_t seed,
           std::uint64_t stream,
           double east_m,
           double north_m,
           double scale_m,
           int octaves) {
    double amplitude=1.0;
    double frequency=1.0/scale_m;
    double total=0.0;
    double normalization=0.0;
    for (int octave=0;octave<octaves;++octave) {
        const auto octave_stream=stream+static_cast<std::uint64_t>(octave)*17ULL;
        total+=amplitude*value_noise(seed,octave_stream,east_m*frequency,north_m*frequency);
        normalization+=amplitude;
        amplitude*=0.5;
        frequency*=2.0;
    }
    return total/normalization;
}

double continent_sdf(std::uint64_t seed, double east_m, double north_m) {
    const double ex=east_m/TerrainGenerator::kContinentSemiMajorM;
    const double ny=north_m/TerrainGenerator::kContinentSemiMinorM;
    const double radial=std::sqrt(ex*ex+ny*ny);
    const double coast_noise=0.10*fbm(seed,100,east_m,north_m,1'600'000.0,4);
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

    const double sdf=continent_sdf(seed_,east_m,north_m);
    const double land_fraction=smoothstep(-0.025,0.025,sdf);
    double elevation=0.0;

    if (sdf>=0.0) {
        const double interior=std::clamp(sdf,0.0,1.0);
        const double rolling=220.0*fbm(seed_,200,east_m,north_m,180'000.0,5);
        const double local_detail=18.0*fbm(seed_,600,east_m,north_m,900.0,4);
        const double ridge_source=fbm(
            seed_,
            300,
            east_m+1'300'000.0,
            north_m-400'000.0,
            520'000.0,
            5
        );
        const double ridge=std::clamp((1.0-std::abs(ridge_source)-0.35)/0.65,0.0,1.0);
        const double uplift=0.5+0.5*fbm(
            seed_,
            400,
            east_m-1'000'000.0,
            north_m+700'000.0,
            900'000.0,
            3
        );
        const double mountain_zone=smoothstep(0.08,0.72,sdf)*smoothstep(0.0,1.0,uplift);
        const double mountains=3'800.0*ridge*ridge*ridge*mountain_zone;
        elevation=120.0+950.0*std::pow(interior,0.75)+rolling+local_detail+mountains;
    } else {
        const double deep=smoothstep(0.0,0.8,-sdf);
        const double ocean_noise=350.0*fbm(seed_,500,east_m,north_m,900'000.0,4);
        elevation=-180.0-4'700.0*deep+ocean_noise*(0.2+0.8*deep);
    }

    return {
        std::clamp(elevation,-11'000.0,9'000.0),
        land_fraction
    };
}

TerrainSample TerrainGenerator::sample_direction(Vec3d direction) const {
    const auto [east_m,north_m]=direction_to_projected(direction);
    return sample_projected(east_m,north_m);
}

} // namespace worldsim
