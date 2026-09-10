#include "worldsim/campsite.hpp"

#include "worldsim/resources.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace worldsim {
namespace {

FieldId require_field(const FieldRegistry& registry,std::string_view key) {
    const auto id=registry.find(key);
    if (!id) throw std::runtime_error("campsite requires field: "+std::string(key));
    return *id;
}

double sample_site_field(
    const Simulation& simulation,
    Vec3d direction,
    std::string_view field_key
) {
    const CellId site=campsite_site_cell(simulation,direction);
    const FieldId field=require_field(simulation.fields(),field_key);
    const auto& world=simulation.world();
    const auto& fields=world.stores().get<FieldStore>();
    double value=0.0;
    for (const ActiveCoverPart& part:world.resolve_active_cover(site))
        value+=fields.get(part.cell,field)*part.weight;
    return value;
}

void require_suitable_site(const Simulation& simulation,Vec3d direction) {
    if (!campsite_site_suitable(simulation,direction))
        throw std::runtime_error("campsite requires non-flooded land");
}

class CampfireBurnSystem final : public ISimSystem {
public:
    [[nodiscard]] std::string_view id() const override {
        return "gameplay.campfire_burn";
    }

    [[nodiscard]] SystemAccess access() const override {
        return {
            {"store:gameplay.campsites"},
            {"store:gameplay.campsites"}
        };
    }

    void step(SystemContext& ctx) override {
        ctx.world.stores().get<CampsiteStore>().burn_lit_campfires(
            ctx.dt_days*24.0
        );
    }
};

} // namespace

void CampsiteStore::save(BinaryWriter& writer) const {
    writer.pod<std::uint64_t>(static_cast<std::uint64_t>(sites_.size()));
    for (const auto& [site,state]:sites_) {
        writer.pod(site.raw());
        writer.pod<std::uint8_t>(state.shelter ? 1U : 0U);
        writer.pod<std::uint8_t>(state.campfire ? 1U : 0U);
        writer.pod<std::uint8_t>(state.campfire_lit ? 1U : 0U);
        writer.pod(state.campfire_fuel_kg);
    }
}

void CampsiteStore::load(BinaryReader& reader,std::uint32_t version) {
    if (version!=1)
        throw std::runtime_error("unsupported gameplay.campsites snapshot version");
    const auto count=reader.pod<std::uint64_t>();
    if (count>1'000'000ULL)
        throw std::runtime_error("campsite snapshot site count is unreasonable");

    std::map<CellId,CampsiteState> staged;
    for (std::uint64_t i=0;i<count;++i) {
        const CellId site(reader.pod<std::uint64_t>());
        const auto shelter=reader.pod<std::uint8_t>();
        const auto campfire=reader.pod<std::uint8_t>();
        const auto lit=reader.pod<std::uint8_t>();
        const double fuel=reader.pod<double>();
        if (!site.valid() || site.level()!=kGameplaySiteLevel)
            throw std::runtime_error("campsite snapshot contains invalid site");
        if (shelter>1U || campfire>1U || lit>1U)
            throw std::runtime_error("campsite snapshot contains invalid flag");
        if (!std::isfinite(fuel) || fuel<0.0 || fuel>1.0e30)
            throw std::runtime_error("campsite snapshot contains invalid fuel");

        CampsiteState state{
            shelter==1U,
            campfire==1U,
            lit==1U,
            fuel
        };
        if (!state.shelter && !state.campfire)
            throw std::runtime_error("campsite snapshot contains empty site");
        if (!state.campfire && (state.campfire_lit || state.campfire_fuel_kg!=0.0))
            throw std::runtime_error("campsite snapshot has fuel without campfire");
        if (state.campfire_lit && !(state.campfire_fuel_kg>0.0))
            throw std::runtime_error("campsite snapshot has lit fire without fuel");
        if (!staged.emplace(site,state).second)
            throw std::runtime_error("campsite snapshot contains duplicate site");
    }
    sites_=std::move(staged);
}

const CampsiteState* CampsiteStore::find(CellId site) const {
    const auto it=sites_.find(site);
    return it==sites_.end() ? nullptr : &it->second;
}

CampsiteState& CampsiteStore::ensure(CellId site) {
    if (!site.valid() || site.level()!=kGameplaySiteLevel)
        throw std::invalid_argument("invalid gameplay campsite cell");
    return sites_[site];
}

void CampsiteStore::burn_lit_campfires(double hours) {
    if (!std::isfinite(hours) || hours<0.0)
        throw std::invalid_argument("campfire burn duration must be finite and non-negative");
    if (!(hours>0.0)) return;
    for (auto& [site,state]:sites_) {
        (void)site;
        if (!state.campfire_lit) continue;
        if (!(state.campfire_fuel_kg>0.0)) {
            state.campfire_fuel_kg=0.0;
            state.campfire_lit=false;
            continue;
        }
        const double burned=std::min(
            state.campfire_fuel_kg,
            kCampfireBurnKgPerHour*hours
        );
        state.campfire_fuel_kg=std::max(
            0.0,
            state.campfire_fuel_kg-burned
        );
        if (!(state.campfire_fuel_kg>0.0))
            state.campfire_lit=false;
    }
}

void CampsiteModule::register_stores(
    StateStoreRegistry& stores,
    const FieldRegistry&
) {
    stores.emplace<CampsiteStore>();
}

void CampsiteModule::register_systems(
    Scheduler& scheduler,
    const FieldRegistry&
) {
    scheduler.add(std::make_unique<CampfireBurnSystem>());
}

CellId campsite_site_cell(
    const Simulation& simulation,
    Vec3d direction
) {
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if (!std::isfinite(length) || !(length>0.0))
        throw std::invalid_argument("campsite direction must be finite and non-zero");
    const Vec3d unit{
        direction.x/length,
        direction.y/length,
        direction.z/length
    };
    return simulation.world().topology().from_direction(
        unit,kGameplaySiteLevel
    );
}

bool campsite_site_suitable(
    const Simulation& simulation,
    Vec3d direction
) {
    const double land=sample_site_field(
        simulation,direction,"geography.land_fraction"
    );
    const double flooded=sample_site_field(
        simulation,direction,"hydrology.flooded_fraction"
    );
    return land>=0.5 && flooded<0.5;
}

CampsiteState campsite_state(
    const Simulation& simulation,
    Vec3d direction
) {
    const CellId site=campsite_site_cell(simulation,direction);
    const auto& store=simulation.world().stores().get<CampsiteStore>();
    if (const CampsiteState* state=store.find(site)) return *state;
    return {};
}

void build_basic_shelter(Simulation& simulation,Vec3d direction) {
    require_suitable_site(simulation,direction);
    const CellId site=campsite_site_cell(simulation,direction);
    auto& store=simulation.world().stores().get<CampsiteStore>();
    if (const CampsiteState* state=store.find(site); state && state->shelter)
        throw std::runtime_error("basic shelter already exists at campsite");

    auto& inventory=simulation.world().stores().get<PlayerInventoryStore>();
    if (!inventory.has_stone_axe())
        throw std::runtime_error("basic shelter requires stone axe");
    if (inventory.amount(ResourceKind::Wood)<kBasicShelterWoodCostKg ||
        inventory.amount(ResourceKind::Stone)<kBasicShelterStoneCostKg) {
        throw std::runtime_error("insufficient materials for basic shelter");
    }

    inventory.consume(ResourceKind::Wood,kBasicShelterWoodCostKg);
    inventory.consume(ResourceKind::Stone,kBasicShelterStoneCostKg);
    store.ensure(site).shelter=true;
    simulation.world().emit({
        simulation.world().tick(),
        "gameplay.shelter_built",
        site,
        0,
        1.0
    });
}

void build_campfire(Simulation& simulation,Vec3d direction) {
    require_suitable_site(simulation,direction);
    const CellId site=campsite_site_cell(simulation,direction);
    auto& store=simulation.world().stores().get<CampsiteStore>();
    if (const CampsiteState* state=store.find(site); state && state->campfire)
        throw std::runtime_error("campfire already exists at campsite");

    auto& inventory=simulation.world().stores().get<PlayerInventoryStore>();
    if (inventory.amount(ResourceKind::Stone)<kCampfireStoneCostKg)
        throw std::runtime_error("insufficient stone for campfire");

    inventory.consume(ResourceKind::Stone,kCampfireStoneCostKg);
    store.ensure(site).campfire=true;
    simulation.world().emit({
        simulation.world().tick(),
        "gameplay.campfire_built",
        site,
        0,
        1.0
    });
}

void add_campfire_fuel(
    Simulation& simulation,
    Vec3d direction,
    double wood_kg
) {
    if (!std::isfinite(wood_kg) || !(wood_kg>0.0))
        throw std::invalid_argument("campfire fuel transfer must be finite and positive");
    const CellId site=campsite_site_cell(simulation,direction);
    auto& store=simulation.world().stores().get<CampsiteStore>();
    CampsiteState* state=nullptr;
    if (const CampsiteState* existing=store.find(site))
        state=&store.ensure(site);
    if (!state || !state->campfire)
        throw std::runtime_error("campfire does not exist at campsite");

    auto& inventory=simulation.world().stores().get<PlayerInventoryStore>();
    if (inventory.amount(ResourceKind::Wood)<wood_kg)
        throw std::runtime_error("insufficient carried wood for campfire fuel");
    const double updated=state->campfire_fuel_kg+wood_kg;
    if (!std::isfinite(updated) || updated>1.0e30)
        throw std::overflow_error("campfire fuel overflow");

    inventory.consume(ResourceKind::Wood,wood_kg);
    state->campfire_fuel_kg=updated;
    simulation.world().emit({
        simulation.world().tick(),
        "gameplay.campfire_fueled",
        site,
        0,
        wood_kg
    });
}

void set_campfire_lit(
    Simulation& simulation,
    Vec3d direction,
    bool lit
) {
    const CellId site=campsite_site_cell(simulation,direction);
    auto& store=simulation.world().stores().get<CampsiteStore>();
    const CampsiteState* existing=store.find(site);
    if (!existing || !existing->campfire)
        throw std::runtime_error("campfire does not exist at campsite");
    CampsiteState& state=store.ensure(site);
    if (lit && !(state.campfire_fuel_kg>0.0))
        throw std::runtime_error("campfire has no fuel");
    state.campfire_lit=lit;
    simulation.world().emit({
        simulation.world().tick(),
        lit ? "gameplay.campfire_lit" : "gameplay.campfire_extinguished",
        site,
        0,
        state.campfire_fuel_kg
    });
}

} // namespace worldsim
