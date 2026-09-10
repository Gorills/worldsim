#include "worldsim/modules.hpp"
#include "worldsim/simulation.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace worldsim;

namespace {

void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<Simulation> make_fixture(SimulationConfig config) {
    auto simulation=std::make_unique<Simulation>(42,config);
    simulation->add_module(std::make_unique<MagicModule>());
    simulation->build();
    return simulation;
}

// Locate the cover without depending on optional focus/command record lengths.
std::size_t cover_count_offset(const std::vector<std::byte>& snapshot) {
    BinaryReader reader(snapshot);
    for (int i=0;i<8;++i) (void)reader.pod<char>();
    (void)reader.pod<std::uint32_t>();
    (void)reader.pod<std::uint64_t>();
    (void)reader.pod<std::uint64_t>();
    (void)reader.pod<std::uint8_t>();
    (void)reader.pod<std::uint8_t>();
    (void)reader.pod<double>();
    (void)reader.pod<Tick>();
    if (reader.pod<std::uint8_t>())
        for (int i=0;i<3;++i) (void)reader.pod<double>();
    (void)reader.pod<std::uint64_t>();
    const auto commands=reader.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<commands;++i) {
        (void)reader.pod<Tick>();
        (void)reader.pod<std::uint64_t>();
        (void)reader.pod<std::uint64_t>();
        (void)reader.pod<FieldId>();
        (void)reader.pod<double>();
    }
    return snapshot.size()-reader.remaining();
}

std::vector<std::byte> replace_cover(
    const std::vector<std::byte>& snapshot,
    const std::vector<CellId>& cells
) {
    const std::size_t offset=cover_count_offset(snapshot);
    BinaryReader reader(std::span<const std::byte>(snapshot).subspan(offset));
    const auto count=reader.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<count;++i) (void)reader.pod<std::uint64_t>();
    const std::size_t end=snapshot.size()-reader.remaining();

    BinaryWriter cover;
    cover.pod<std::uint64_t>(static_cast<std::uint64_t>(cells.size()));
    for (CellId cell:cells) cover.pod(cell.raw());
    std::vector<std::byte> result(snapshot.begin(),snapshot.begin()+static_cast<std::ptrdiff_t>(offset));
    result.insert(result.end(),cover.data().begin(),cover.data().end());
    result.insert(result.end(),snapshot.begin()+static_cast<std::ptrdiff_t>(end),snapshot.end());
    return result;
}

void uniform_cover_roundtrip() {
    for (const auto level:{std::uint8_t{0},std::uint8_t{4}}) {
        auto original=make_fixture({level,level,3600.0});
        const auto snapshot=original->save_snapshot();
        auto restored=make_fixture(original->config());
        restored->load_snapshot(snapshot);
        check(restored->save_snapshot()==snapshot,"uniform cover changed on restore");
    }
}

void sparse_deep_cover_roundtrip_and_rejected_covers() {
    auto original=make_fixture({0,CellId::kMaxLevel,3600.0});
    auto& world=original->world();
    // Highly non-uniform covers are legal; do not introduce a 2:1 constraint.
    for (std::uint8_t face=0;face<6;++face) {
        CellId cell=CellId::make(face,0,0,0);
        const std::uint8_t depth=face==0 ? CellId::kMaxLevel : face;
        for (std::uint8_t level=0;level<depth;++level) {
            world.refine(cell);
            cell=cell.children()[face%4U];
        }
    }
    const auto expected_cover=world.active_cells();
    const CellId target=*expected_cover.rbegin();
    original->schedule_field_impulse(0,target,"magic.mana_j",123.0);
    world.emit({0,"test.cover_restore",target,7,2.0});
    const auto snapshot=original->save_snapshot();

    auto restored=make_fixture(original->config());
    restored->load_snapshot(snapshot);
    check(restored->world().active_cells()==expected_cover,"deep mixed cover changed on restore");
    check(restored->save_snapshot()==snapshot,"deep mixed snapshot did not round-trip exactly");

    auto reject_unchanged=[&](std::vector<CellId> cells) {
        bool rejected=false;
        try { restored->load_snapshot(replace_cover(snapshot,cells)); }
        catch (const std::exception&) { rejected=true; }
        check(rejected,"invalid hierarchy cover was accepted");
        check(restored->save_snapshot()==snapshot,"rejected cover changed live state");
    };
    std::vector<CellId> cells(expected_cover.begin(),expected_cover.end());
    reject_unchanged({});
    auto missing=cells;
    missing.pop_back();
    reject_unchanged(missing);
    auto overlapping=cells;
    overlapping.push_back(CellId::make(0,0,0,0));
    reject_unchanged(overlapping);
    auto duplicate=cells;
    duplicate.push_back(cells.back());
    reject_unchanged(duplicate);

    // Compare against an uninterrupted world, not two reloaded copies.
    original->step(1);
    restored->step(1);
    check(original->save_snapshot()==restored->save_snapshot(),"restore changed command/event/LOD continuation");
}

void large_adaptive_cover_roundtrip() {
    auto original=make_fixture({4,10,3600.0});
    original->set_focus({1.0,0.2,0.1});
    original->step(1);
    const auto count=original->world().active_cells().size();
    check(count>100'000U,"large-cover fixture did not exercise scaling regression");
    const auto snapshot=original->save_snapshot();
    auto restored=make_fixture(original->config());
    const auto start=std::chrono::steady_clock::now();
    restored->load_snapshot(snapshot);
    const double seconds=std::chrono::duration<double>(
        std::chrono::steady_clock::now()-start
    ).count();
    check(restored->save_snapshot()==snapshot,"large adaptive snapshot did not round-trip exactly");
    std::cout << "restored " << count << " adaptive cells in " << seconds << " seconds\n";
    // CTest supplies a generous process timeout rather than a brittle per-host
    // timing assertion. The former all-pairs ancestor search exceeds that budget.
}

} // namespace

int main() {
    try {
        uniform_cover_roundtrip();
        sparse_deep_cover_roundtrip_and_rejected_covers();
        large_adaptive_cover_roundtrip();
        std::cout << "snapshot cover tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "snapshot cover test failure: " << error.what() << '\n';
        return 1;
    }
}
