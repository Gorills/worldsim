#include "worldsim/tectonics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace worldsim {
namespace {

constexpr double kGoldenAngleRad=2.3999632297286533222;
constexpr double kMacroCompetitionScoreGap=0.10;
constexpr double kPlateLayoutJitterRad=1.0;
constexpr double kPlateMinimumSeedSeparationRad=20.0*kPi/180.0;
constexpr std::array<double,5> kPlateJitterBackoffFactors{1.0,0.75,0.5,0.25,0.0};
constexpr std::uint32_t kCrustCalibrationSamples=128;

double smoothstep01(double x) {
    const double t=std::clamp(x,0.0,1.0);
    return t*t*(3.0-2.0*t);
}

double smoothstep(double a, double b, double x) {
    return smoothstep01((x-a)/(b-a));
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

double crust_fbm(std::uint64_t seed, Vec3d p) {
    constexpr int kOctaves=3;
    constexpr double kBaseFrequency=1.35;
    constexpr double kPersistence=0.5;
    const std::uint64_t stream=fnv1a64("tectonics.crust.field");

    double amplitude=1.0;
    double frequency=kBaseFrequency;
    double total=0.0;
    double normalization=0.0;
    for (int octave=0;octave<kOctaves;++octave) {
        total+=amplitude*value_noise3(
            seed,
            stream+static_cast<std::uint64_t>(octave)*17ULL,
            p.x*frequency,
            p.y*frequency,
            p.z*frequency
        );
        normalization+=amplitude;
        amplitude*=kPersistence;
        frequency*=2.0;
    }
    return total/normalization;
}

double orogeny_fbm(std::uint64_t seed,
                   std::uint64_t stream,
                   Vec3d p,
                   double base_frequency) {
    constexpr int kOctaves=3;
    constexpr double kPersistence=0.5;

    double amplitude=1.0;
    double frequency=base_frequency;
    double total=0.0;
    double normalization=0.0;
    for (int octave=0;octave<kOctaves;++octave) {
        total+=amplitude*value_noise3(
            seed,
            stream+static_cast<std::uint64_t>(octave)*17ULL,
            p.x*frequency,
            p.y*frequency,
            p.z*frequency
        );
        normalization+=amplitude;
        amplitude*=kPersistence;
        frequency*=2.0;
    }
    return total/normalization;
}

double orogenic_width_factor(std::uint64_t seed, Vec3d p) {
    const double raw=orogeny_fbm(
        seed,
        fnv1a64("tectonics.orogeny.width"),
        p,
        3.0
    );
    const double noise01=std::clamp(0.5+0.5*raw,0.0,1.0);
    return 0.50+0.48*noise01;
}

double orogenic_ridge_modulation(std::uint64_t seed, Vec3d p) {
    const double raw=orogeny_fbm(
        seed,
        fnv1a64("tectonics.orogeny.ridges"),
        p,
        5.0
    );
    double ridge=std::clamp(1.0-2.0*std::abs(raw),0.0,1.0);
    ridge*=ridge;
    return 0.45+0.75*ridge;
}

double crust_field_bias(std::uint64_t seed) {
    // The lowest-frequency octave spans only a few lattice cells across the
    // sphere, so its spherical mean can drift substantially between seeds.
    // Remove that seed-wide DC component with a small deterministic equal-area
    // probe; local morphology and continuity are unchanged.
    double mean=0.0;
    for (std::uint32_t i=0;i<kCrustCalibrationSamples;++i) {
        const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
            static_cast<double>(kCrustCalibrationSamples);
        const double phi=static_cast<double>(i)*kGoldenAngleRad;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        mean+=crust_fbm(seed,{
            radial*std::cos(phi),
            radial*std::sin(phi),
            z
        });
    }
    return -mean/static_cast<double>(kCrustCalibrationSamples);
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

TectonicModel::TectonicModel(std::uint64_t seed):
    seed_(seed),
    crust_bias_(crust_field_bias(seed)) {
    const double phase=2.0*kPi*deterministic_unit(
        seed,
        fnv1a64("tectonics.layout.phase"),
        0,
        0
    );

    std::array<Vec3d,kPlateCount> base_directions{};
    for (std::uint32_t i=0;i<kPlateCount;++i) {
        const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
            static_cast<double>(kPlateCount);
        const double phi=phase+static_cast<double>(i)*kGoldenAngleRad;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        base_directions[i]={radial*std::cos(phi),radial*std::sin(phi),z};
    }

    const double maximum_seed_dot=std::cos(kPlateMinimumSeedSeparationRad);
    for (std::uint32_t i=0;i<kPlateCount;++i) {
        const Vec3d base=base_directions[i];
        const Vec3d tangent=seeded_tangent(
            seed,
            fnv1a64("tectonics.layout.jitter"),
            i,
            base
        );
        const double desired_jitter_angle=(
            deterministic_unit(seed,fnv1a64("tectonics.layout.angle"),0,i)-0.5
        )*kPlateLayoutJitterRad;

        // Large jitter is useful for plate-area diversity, but unconstrained
        // jitter can place two seeds almost on top of each other. Back off only
        // when needed. Every accepted seed also stays clear of all remaining
        // Fibonacci anchors, so factor 0 (the original anchor) remains a safe
        // deterministic fallback for later plates.
        Vec3d seed_direction=base;
        for (double factor:kPlateJitterBackoffFactors) {
            const double jitter_angle=desired_jitter_angle*factor;
            const Vec3d candidate=normalized(
                base*std::cos(jitter_angle)+tangent*std::sin(jitter_angle)
            );

            bool separated=true;
            for (std::uint32_t j=0;j<i && separated;++j) {
                separated=dot(candidate,plates_[j].seed_direction)<=maximum_seed_dot;
            }
            for (std::uint32_t j=0;j<kPlateCount && separated;++j) {
                if (j==i) continue;
                separated=dot(candidate,base_directions[j])<=maximum_seed_dot;
            }
            if (!separated) continue;
            seed_direction=candidate;
            break;
        }

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
}

double TectonicModel::continental_affinity(Vec3d unit_direction) const {
    // Evaluate low-frequency 3D FBM directly on the unit sphere. This keeps the
    // field continuous and seam-free without encoding continent silhouettes as
    // unions of radial spherical caps.
    const double field=crust_fbm(seed_,unit_direction)+crust_bias_;
    return smoothstep(0.03,0.27,field);
}

TectonicSample TectonicModel::sample_direction(Vec3d direction) const {
    const Vec3d p=normalized(direction);

    std::array<double,kPlateCount> plate_scores{};
    std::uint32_t owner=0;
    for (std::uint32_t i=0;i<kPlateCount;++i)
        plate_scores[i]=dot(p,plates_[i].seed_direction);

    double owner_score=plate_scores[owner];
    for (std::uint32_t i=1;i<kPlateCount;++i) {
        if (plate_scores[i]>owner_score) {
            owner=i;
            owner_score=plate_scores[i];
        }
    }

    // The runner-up ownership score is not necessarily the nearest spherical
    // Voronoi boundary: pair bisector distance also depends on the angular
    // separation of the two plate seeds. Compare actual bisector distances.
    std::uint32_t neighbor=owner==0 ? 1 : 0;
    BoundaryMetrics nearest_boundary=boundary_metrics(
        p,
        plates_[owner],
        plates_[neighbor]
    );
    for (std::uint32_t i=0;i<kPlateCount;++i) {
        if (i==owner || i==neighbor) continue;
        const BoundaryMetrics candidate=boundary_metrics(
            p,
            plates_[owner],
            plates_[i]
        );
        if (candidate.distance_rad<nearest_boundary.distance_rad) {
            neighbor=i;
            nearest_boundary=candidate;
        }
    }
    const double boundary_influence=
        1.0-smoothstep01(nearest_boundary.distance_rad/kBoundaryInfluenceRad);
    const double boundary_forcing=nearest_boundary.convergence*boundary_influence;

    // Weight each pair by how close both of its plates are to local ownership.
    // The compact smoothstep reaches zero with zero slope, so skipping a zero-
    // weight pair is only a performance shortcut, not a hard selection seam.
    // Non-neighbor pair bisectors are suppressed once another plate dominates.
    // Convergent belts use a narrower, spatially varying envelope and a
    // sphere-native ridged modulation. Both fields are continuous and are
    // multiplied by the actual convergent boundary response, so texture can
    // segment and branch an orogen but cannot create isolated mountains away
    // from tectonic convergence. Divergence keeps the broader 12-degree belt.
    const double uplift_width=
        kMacroBoundaryInfluenceRad*orogenic_width_factor(seed_,p);
    const double uplift_modulation=orogenic_ridge_modulation(seed_,p);

    double uplift_sum=0.0;
    double divergence_sum=0.0;
    for (std::uint32_t a=0;a<kPlateCount;++a) {
        for (std::uint32_t b=a+1;b<kPlateCount;++b) {
            const double pair_score=std::min(plate_scores[a],plate_scores[b]);
            const double competition_gap=std::max(0.0,owner_score-pair_score);
            const double competition_weight=
                1.0-smoothstep01(competition_gap/kMacroCompetitionScoreGap);
            if (competition_weight<=0.0) continue;

            const BoundaryMetrics metrics=boundary_metrics(p,plates_[a],plates_[b]);
            const double divergence_weight=
                1.0-smoothstep01(metrics.distance_rad/kMacroBoundaryInfluenceRad);
            const double uplift_weight=
                1.0-smoothstep01(metrics.distance_rad/uplift_width);

            uplift_sum+=
                std::max(metrics.convergence,0.0)*
                competition_weight*
                uplift_weight*
                uplift_modulation;
            divergence_sum+=
                std::max(-metrics.convergence,0.0)*
                competition_weight*
                divergence_weight;
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
