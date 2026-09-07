#include "worldsim/tectonics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

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

Vec3d seeded_tangent(std::uint64_t seed,
                     std::uint64_t stream,
                     std::uint64_t object,
                     Vec3d normal) {
    const Vec3d source=seeded_unit_vector(seed,stream,object);
    const Vec3d tangent=source-normal*dot(source,normal);
    return norm(tangent)<1.0e-9 ? fallback_tangent(normal) : normalized(tangent);
}

struct BoundaryMetrics {
    double distance_rad{};
    double convergence{};
    double shear{};
};

BoundaryMetrics boundary_metrics(Vec3d p,
                                 const TectonicPlate& first,
                                 const TectonicPlate& second) {
    const Vec3d plane_normal=normalized(first.seed_direction-second.seed_direction);
    const double signed_side=std::clamp(dot(p,plane_normal),-1.0,1.0);
    const double distance=std::asin(std::abs(signed_side));

    Vec3d boundary_normal=plane_normal-p*dot(p,plane_normal);
    if (norm(boundary_normal)<1.0e-12) boundary_normal=fallback_tangent(p);
    else boundary_normal=normalized(boundary_normal);

    const Vec3d first_velocity=cross(first.angular_velocity,p);
    const Vec3d second_velocity=cross(second.angular_velocity,p);
    const Vec3d relative_velocity=second_velocity-first_velocity;

    // Reversing first/second flips both relative_velocity and boundary_normal,
    // so the convergence sign is pair-order invariant.
    const double convergence=std::clamp(
        dot(relative_velocity,boundary_normal)*0.5,
        -1.0,
        1.0
    );
    const Vec3d boundary_tangent=normalized(cross(p,boundary_normal));
    const double shear=std::clamp(
        dot(relative_velocity,boundary_tangent)*0.5,
        -1.0,
        1.0
    );
    return {distance,convergence,shear};
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

        const Vec3d tangent=seeded_tangent(
            seed,
            fnv1a64("tectonics.layout.jitter"),
            i,
            base
        );
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
            rotation_axis*speed
        };
    }

    // Five broad continental provinces are anchored near distinct plate seeds,
    // but each province is a union of overlapping smooth spherical lobes. The
    // lobes may cross Voronoi boundaries, so continental crust cannot collapse
    // back into a per-plate boolean.
    const std::uint32_t anchor_offset=static_cast<std::uint32_t>(
        deterministic_unit(seed,fnv1a64("tectonics.crust.anchor"),0,0)*
        static_cast<double>(kPlateCount)
    );
    std::uint32_t lobe_index=0;
    for (std::uint32_t province=0;province<kCrustProvinceCount;++province) {
        const std::uint32_t plate_id=(anchor_offset+province*3U)%kPlateCount;
        const Vec3d base=plates_[plate_id].seed_direction;
        const Vec3d primary_tangent=seeded_tangent(
            seed,
            fnv1a64("tectonics.crust.primary.offset"),
            province,
            base
        );
        const double primary_angle=(
            deterministic_unit(seed,fnv1a64("tectonics.crust.primary.angle"),0,province)-0.5
        )*0.28;
        const Vec3d primary=normalized(
            base*std::cos(primary_angle)+primary_tangent*std::sin(primary_angle)
        );

        for (std::uint32_t local=0;local<kCrustLobesPerProvince;++local) {
            Vec3d center=primary;
            if (local>0) {
                const Vec3d tangent=seeded_tangent(
                    seed,
                    fnv1a64("tectonics.crust.lobe.offset")+static_cast<std::uint64_t>(local)*17ULL,
                    province,
                    primary
                );
                const double offset_angle=0.13+0.18*deterministic_unit(
                    seed,
                    fnv1a64("tectonics.crust.lobe.angle")+static_cast<std::uint64_t>(local)*19ULL,
                    0,
                    province
                );
                center=normalized(
                    primary*std::cos(offset_angle)+tangent*std::sin(offset_angle)
                );
            }

            const double radius=0.30+0.16*deterministic_unit(
                seed,
                fnv1a64("tectonics.crust.lobe.radius")+static_cast<std::uint64_t>(local)*23ULL,
                0,
                province
            );
            constexpr double edge_width=0.07;
            crust_lobes_[lobe_index++]={
                center,
                std::cos(radius-edge_width),
                std::cos(radius+edge_width)
            };
        }
    }
}

double TectonicModel::continental_affinity(Vec3d unit_direction) const {
    double oceanic_remainder=1.0;
    for (const CrustLobe& lobe:crust_lobes_) {
        const double denominator=lobe.inner_cos-lobe.outer_cos;
        const double raw=(dot(unit_direction,lobe.center)-lobe.outer_cos)/denominator;
        const double affinity=smoothstep01(raw);
        oceanic_remainder*=1.0-affinity;
    }
    return std::clamp(1.0-oceanic_remainder,0.0,1.0);
}

TectonicSample TectonicModel::sample_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);

    std::array<double,kPlateCount> plate_scores{};
    std::uint32_t owner=0;
    std::uint32_t neighbor=1;
    for (std::uint32_t i=0;i<kPlateCount;++i)
        plate_scores[i]=dot(p,plates_[i].seed_direction);

    double owner_score=plate_scores[owner];
    double neighbor_score=plate_scores[neighbor];
    if (neighbor_score>owner_score) {
        std::swap(owner,neighbor);
        std::swap(owner_score,neighbor_score);
    }
    for (std::uint32_t i=2;i<kPlateCount;++i) {
        const double score=plate_scores[i];
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

    const BoundaryMetrics nearest_boundary=boundary_metrics(
        p,
        plates_[owner],
        plates_[neighbor]
    );
    const double boundary_influence=
        1.0-smoothstep01(nearest_boundary.distance_rad/kBoundaryInfluenceRad);
    const double boundary_forcing=nearest_boundary.convergence*boundary_influence;

    // Macro response blends all locally competitive plate pairs. Pair metrics
    // are order-invariant, and the score gate is wider than the 12-degree
    // influence belt for ordinary neighboring plates. This avoids imprinting
    // owner/second/third-neighbor switches as hard relief seams.
    constexpr double kMacroCompetitionScoreGap=0.45;
    double uplift_sum=0.0;
    double divergence_sum=0.0;
    for (std::uint32_t a=0;a<kPlateCount;++a) {
        if (plate_scores[a]<owner_score-kMacroCompetitionScoreGap) continue;
        for (std::uint32_t b=a+1;b<kPlateCount;++b) {
            if (plate_scores[b]<owner_score-kMacroCompetitionScoreGap) continue;
            const BoundaryMetrics metrics=boundary_metrics(p,plates_[a],plates_[b]);
            const double influence=
                1.0-smoothstep01(metrics.distance_rad/kMacroBoundaryInfluenceRad);
            uplift_sum+=std::max(metrics.convergence,0.0)*influence;
            divergence_sum+=std::max(-metrics.convergence,0.0)*influence;
        }
    }

    const double uplift=1.0-std::exp(-uplift_sum);
    const double divergence=1.0-std::exp(-divergence_sum);
    const double affinity=continental_affinity(p);

    // Preview-only macro hypsometry. Continental affinity controls buoyancy;
    // convergence raises both crust types but more strongly on continental
    // crust; divergence raises oceanic ridges and lowers continental rifts.
    const double buoyancy=smoothstep01((affinity-0.30)/0.50);
    const double base_elevation=-4'500.0+5'200.0*buoyancy;
    const double convergent_uplift=(1'700.0+2'600.0*affinity)*uplift;
    const double divergent_response=(
        1'800.0*(1.0-affinity)-1'100.0*affinity
    )*divergence;
    const double macro_elevation=std::clamp(
        base_elevation+convergent_uplift+divergent_response,
        -6'000.0,
        6'500.0
    );

    return {
        owner,
        neighbor,
        nearest_boundary.distance_rad,
        nearest_boundary.convergence,
        nearest_boundary.shear,
        boundary_forcing,
        affinity,
        uplift,
        divergence,
        macro_elevation
    };
}

} // namespace worldsim
