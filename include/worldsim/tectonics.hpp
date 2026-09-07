#pragma once

#include "worldsim/types.hpp"

#include <array>
#include <cstdint>

namespace worldsim {

struct TectonicPlate {
    std::uint32_t id{};
    Vec3d seed_direction{};
    // Relative normalized rotation vector for boundary classification; not SI angular velocity.
    Vec3d angular_velocity{};
};

struct TectonicSample {
    std::uint32_t plate_id{};
    std::uint32_t neighbor_plate_id{};
    double boundary_distance_rad{};
    double convergence{};
    double shear{};
    double boundary_forcing{};

    // Continuous [0,1] crust field. It is intentionally not a per-plate boolean:
    // one plate may contain both oceanic and continental crust.
    double continental_affinity{};

    // Positive normalized diagnostic fields used only for the macro-relief preview.
    double uplift_forcing{};
    double divergence_forcing{};

    // Preview height derived from crust buoyancy and tectonic boundary response.
    // It is not yet authoritative geography.elevation_m.
    double macro_elevation_m{};
};

// Deterministic, query-only spherical plate/crust model. Plate ownership is a
// spherical Voronoi partition; crust affinity is a separate smooth spherical
// field, and neither currently modifies authoritative terrain elevation.
class TectonicModel {
public:
    static constexpr std::uint32_t kPlateCount=16;
    // Legacy source-compatibility value from the first debug preview. The
    // current crust field no longer uses discrete provinces.
    static constexpr std::uint32_t kCrustProvinceCount=5;
    static constexpr double kBoundaryInfluenceRad=8.0*kPi/180.0;
    static constexpr double kMacroBoundaryInfluenceRad=12.0*kPi/180.0;

    explicit TectonicModel(std::uint64_t seed);

    [[nodiscard]] TectonicSample sample_direction(Vec3d direction) const;
    [[nodiscard]] const std::array<TectonicPlate,kPlateCount>& plates() const { return plates_; }

private:
    [[nodiscard]] double continental_affinity(Vec3d unit_direction) const;

    std::uint64_t seed_{};
    std::array<TectonicPlate,kPlateCount> plates_{};
};

} // namespace worldsim
