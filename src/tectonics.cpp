#include "worldsim/tectonics.hpp"

#include <algorithm>
#include <cmath>

namespace worldsim {
namespace {

constexpr double kGoldenAngleRad=2.3999632297286533222;

double smoothstep01(double x) {
    const double t=std::clamp(x,0.0,1.0);
    return t*t*(3.0-2.0*t);
}

Vec3d seeded_unit_vector(std::uint64_t seed, std::uint64_t stream, std::uint64_t object) {
    const double z=2.0*deterministic_unit(seed,stream,0,object)-1.0;
    const double phi=2.0*kPi*deterministic_unit(seed,stream+1,0,object);
    const double radial=std::sqrt(std::max(0.0,1.0-z*z));
    return {radial*std::cos(phi),radial*std::sin(phi),z};
}

Vec3d fallback_tangent(Vec3d p) {
    Vec3d tangent=cross(p,{0.0,0.0,1.0});
    if (norm(tangent)<1.0e-9) tangent=cross(p,{0.0,1.0,0.0});
    return normalized(tangent);
}

} // namespace

TectonicModel::TectonicModel(std::uint64_t seed) {
    const double phase=2.0*kPi*deterministic_unit(
        seed,
        fnv1a64("tectonics.layout.phase"),
        0,
        0
    );

    for (std::uint32_t i=0;i<kPlateCount;++i) {
        const double z=1.0-2.0*(static_cast<double>(i)+0.5)/static_cast<double>(kPlateCount);
        const double phi=phase+static_cast<double>(i)*kGoldenAngleRad;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        const Vec3d base{radial*std::cos(phi),radial*std::sin(phi),z};

        const Vec3d jitter_source=seeded_unit_vector(
            seed,
            fnv1a64("tectonics.layout.jitter"),
            i
        );
        Vec3d tangent=jitter_source-base*dot(jitter_source,base);
        if (norm(tangent)<1.0e-9) tangent=fallback_tangent(base);
        else tangent=normalized(tangent);
        const double jitter_angle=(
            deterministic_unit(seed,fnv1a64("tectonics.layout.angle"),0,i)-0.5
        )*0.24;
        const Vec3d seed_direction=normalized(
            base*std::cos(jitter_angle)+tangent*std::sin(jitter_angle)
        );

        const Vec3d rotation_axis=seeded_unit_vector(
            seed,
            fnv1a64("tectonics.rotation.axis"),
            i
        );
        const double speed=0.35+0.65*deterministic_unit(
            seed,
            fnv1a64("tectonics.rotation.speed"),
            0,
            i
        );

        plates_[i]={
            i,
            seed_direction,
            rotation_axis*speed,
            deterministic_unit(seed,fnv1a64("tectonics.crust.type"),0,i)<0.35
        };
    }
}

TectonicSample TectonicModel::sample_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);

    std::uint32_t owner=0;
    std::uint32_t neighbor=1;
    double owner_score=dot(p,plates_[owner].seed_direction);
    double neighbor_score=dot(p,plates_[neighbor].seed_direction);
    if (neighbor_score>owner_score) {
        std::swap(owner,neighbor);
        std::swap(owner_score,neighbor_score);
    }

    for (std::uint32_t i=2;i<kPlateCount;++i) {
        const double score=dot(p,plates_[i].seed_direction);
        if (score>owner_score) {
            neighbor=owner;
            neighbor_score=owner_score;
            owner=i;
            owner_score=score;
        } else if (score>neighbor_score) {
            neighbor=i;
            neighbor_score=score;
        }
    }

    const Vec3d difference=plates_[owner].seed_direction-plates_[neighbor].seed_direction;
    const Vec3d plane_normal=normalized(difference);
    const double side=std::clamp(dot(p,plane_normal),0.0,1.0);
    const double best_distance=std::asin(side);

    Vec3d boundary_normal=plane_normal-p*dot(p,plane_normal);
    if (norm(boundary_normal)<1.0e-12) boundary_normal=fallback_tangent(p);
    else boundary_normal=normalized(boundary_normal);

    const Vec3d owner_velocity=cross(plates_[owner].angular_velocity,p);
    const Vec3d neighbor_velocity=cross(plates_[neighbor].angular_velocity,p);
    const Vec3d relative_velocity=neighbor_velocity-owner_velocity;

    // boundary_normal points from the neighboring plate toward the owner.
    // Positive convergence means both sides close across the boundary.
    const double convergence=std::clamp(dot(relative_velocity,boundary_normal)*0.5,-1.0,1.0);
    const Vec3d boundary_tangent=normalized(cross(p,boundary_normal));
    const double shear=std::clamp(dot(relative_velocity,boundary_tangent)*0.5,-1.0,1.0);

    const double influence=1.0-smoothstep01(best_distance/kBoundaryInfluenceRad);
    const double forcing=convergence*influence;

    return {
        owner,
        neighbor,
        best_distance,
        convergence,
        shear,
        forcing,
        plates_[owner].continental
    };
}

} // namespace worldsim
