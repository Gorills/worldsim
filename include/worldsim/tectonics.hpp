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
    bool continental{};
};

struct TectonicSample {
    std::uint32_t plate_id{};
    std::uint32_t neighbor_plate_id{};
    double boundary_distance_rad{};
    double convergence{};
    double shear{};
    double boundary_forcing{};
    bool continental{};
};

// Deterministic, query-only spherical plate model. Plate ownership is a
// spherical Voronoi partition; kinematics come from per-plate angular velocity.
// This model does not modify terrain elevation yet.
class TectonicModel {
public:
    static constexpr std::uint32_t kPlateCount=16;
    static constexpr double kBoundaryInfluenceRad=8.0*kPi/180.0;

    explicit TectonicModel(std::uint64_t seed);

    [[nodiscard]] TectonicSample sample_direction(Vec3d direction) const;
    [[nodiscard]] const std::array<TectonicPlate,kPlateCount>& plates() const { return plates_; }

private:
    std::array<TectonicPlate,kPlateCount> plates_{};
};

} // namespace worldsim
