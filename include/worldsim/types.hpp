#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace worldsim {

using Tick = std::uint64_t;
using FieldId = std::uint32_t;

struct Vec3d {
    double x{};
    double y{};
    double z{};
};

inline Vec3d operator+(Vec3d a, Vec3d b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec3d operator-(Vec3d a, Vec3d b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec3d operator*(Vec3d a, double s) { return {a.x*s,a.y*s,a.z*s}; }
inline double dot(Vec3d a, Vec3d b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3d cross(Vec3d a, Vec3d b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline double norm(Vec3d a) { return std::sqrt(dot(a,a)); }
inline Vec3d normalized(Vec3d a) {
    const double n=norm(a);
    return n>0.0 ? a*(1.0/n) : Vec3d{1.0,0.0,0.0};
}

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kEarthRadiusM = 6'371'000.0;

inline std::uint64_t fnv1a64(std::string_view s) {
    std::uint64_t h=1469598103934665603ULL;
    for (char ch:s) {
        const auto c=static_cast<unsigned char>(ch);
        h^=c;
        h*=1099511628211ULL;
    }
    return h;
}

inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

inline double deterministic_unit(std::uint64_t seed, std::uint64_t stream, Tick tick, std::uint64_t object) {
    const std::uint64_t h=mix64(seed ^ mix64(stream) ^ mix64(tick) ^ mix64(object));
    return static_cast<double>(h >> 11U) * (1.0 / 9007199254740992.0);
}

} // namespace worldsim
