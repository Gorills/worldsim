#pragma once

#include "worldsim/types.hpp"
#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

namespace worldsim {

class CellId {
public:
    static constexpr std::uint8_t kMaxLevel=20;
    CellId()=default;
    explicit constexpr CellId(std::uint64_t raw): raw_(raw) {}
    static CellId make(std::uint8_t face, std::uint8_t level, std::uint32_t x, std::uint32_t y);

    [[nodiscard]] constexpr std::uint64_t raw() const { return raw_; }
    [[nodiscard]] std::uint8_t face() const;
    [[nodiscard]] std::uint8_t level() const;
    [[nodiscard]] std::uint32_t x() const;
    [[nodiscard]] std::uint32_t y() const;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] CellId parent() const;
    [[nodiscard]] std::array<CellId,4> children() const;

    friend constexpr auto operator<=>(const CellId&, const CellId&)=default;
private:
    std::uint64_t raw_{};
};

class CubeSphereTopology {
public:
    [[nodiscard]] Vec3d center_unit(CellId id) const;
    [[nodiscard]] std::array<Vec3d,4> corners_unit(CellId id) const;
    [[nodiscard]] double area_m2(CellId id) const;
    [[nodiscard]] double edge_length_m(CellId id, std::size_t side) const;
    [[nodiscard]] double shared_boundary_length_m(CellId a, CellId b) const;
    [[nodiscard]] std::array<CellId,4> neighbors4(CellId id) const;
    [[nodiscard]] CellId from_direction(Vec3d direction, std::uint8_t level) const;
    [[nodiscard]] std::pair<double,double> lat_lon_rad(CellId id) const;

private:
    static Vec3d face_uv_to_xyz(std::uint8_t face, double u, double v);
    static std::tuple<std::uint8_t,double,double> xyz_to_face_uv(Vec3d p);
    static double spherical_triangle_area(Vec3d a, Vec3d b, Vec3d c);
};

std::set<CellId> uniform_cover(std::uint8_t level);

} // namespace worldsim
