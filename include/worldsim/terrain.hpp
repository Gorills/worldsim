#pragma once

#include "worldsim/types.hpp"

#include <cstdint>
#include <utility>

namespace worldsim {

struct TerrainSample {
    double elevation_m{};
    double land_fraction{};
};

// Deterministic, stateless geography source shared by simulation state and engine adapters.
// Projected coordinates use an azimuthal-equidistant map centered on the initial continent.
class TerrainGenerator {
public:
    explicit TerrainGenerator(std::uint64_t seed): seed_(seed) {}

    [[nodiscard]] TerrainSample sample_projected(double east_m, double north_m) const;
    [[nodiscard]] TerrainSample sample_direction(Vec3d direction) const;

    [[nodiscard]] static Vec3d projected_to_direction(double east_m, double north_m);
    [[nodiscard]] static std::pair<double,double> direction_to_projected(Vec3d direction);

    static constexpr double kContinentSemiMajorM=6'500'000.0;
    static constexpr double kContinentSemiMinorM=2'700'000.0;

private:
    std::uint64_t seed_{};
};

} // namespace worldsim
