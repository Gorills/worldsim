#pragma once

#include "worldsim/tectonics.hpp"
#include "worldsim/types.hpp"

#include <cstdint>
#include <utility>

namespace worldsim {

struct TerrainSample {
    double elevation_m{};
    double land_fraction{};
};

// Deterministic sphere-native geography source shared by simulation state and
// engine adapters. Tectonics owns the broad elevation basis; procedural noise
// adds bounded meso/local detail. Projected coordinates are only a local walker adapter:
// sample_projected() maps them back to a sphere direction and delegates to sample_direction().
class TerrainGenerator {
public:
    explicit TerrainGenerator(std::uint64_t seed): seed_(seed), tectonics_(seed) {}

    [[nodiscard]] TerrainSample sample_projected(double east_m, double north_m) const;
    [[nodiscard]] TerrainSample sample_direction(Vec3d direction) const;

    [[nodiscard]] static Vec3d projected_to_direction(double east_m, double north_m);
    [[nodiscard]] static std::pair<double,double> direction_to_projected(Vec3d direction);

    // Legacy source-compatibility values from the initial single-continent
    // terrain slice. Authoritative terrain no longer uses a fixed cap.
    static constexpr double kContinentSemiMajorM=6'500'000.0;
    static constexpr double kContinentSemiMinorM=2'700'000.0;

private:
    std::uint64_t seed_{};
    TectonicModel tectonics_;
};

} // namespace worldsim
