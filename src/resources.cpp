#include "worldsim/resources.hpp"

#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace worldsim {
namespace {

constexpr double kVegetationCarbonFractionOfDryMass=0.45;

FieldId require_field(const FieldRegistry& registry,std::string_view key) {
    const auto id=registry.find(key);
    if (!id) throw std::runtime_error("resources require field: "+std::string(key));
    return *id;
}

std::size_t resource_index(ResourceKind kind) {
    const auto index=static_cast<std::size_t>(kind);
    if (index>=kResourceDescriptors.size())
        throw std::invalid_argument("invalid resource kind");
    return index;
}

std::string_view stock_field_key(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::PlantFood: return "resources.plant_food_kg";
        case ResourceKind::Wood: return "resources.wood_kg";
        case ResourceKind::Stone: return "resources.stone_kg";
        case ResourceKind::MetalOre: return "resources.metal_ore_kg";
        case ResourceKind::FreshWater:
        case ResourceKind::Count:
            break;
    }
    throw std::invalid_argument("resource does not use a resource stock field");
}

CellId interaction_region(const Simulation& simulation,Vec3d direction) {
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if (!std::isfinite(length) || !(length>0.0))
        throw std::invalid_argument("resource direction must be finite and non-zero");
    const Vec3d unit{direction.x/length,direction.y/length,direction.z/length};
    return simulation.world().topology().from_direction(
        unit,
        simulation.config().base_level
    );
}

double sum_extensive_region(
    const WorldState& world,
    const FieldStore& fields,
    FieldId field,
    CellId region
) {
    double total=0.0;
    for (const ActiveCoverPart& part:world.resolve_active_cover(region))
        total+=fields.get(part.cell,field);
    return total;
}

void debit_extensive_region(
    WorldState& world,
    FieldStore& fields,
    FieldId field,
    CellId region,
    double amount
) {
    if (!(amount>=0.0) || !std::isfinite(amount))
        throw std::invalid_argument("resource debit must be finite and non-negative");
    if (!(amount>0.0)) return;

    const auto parts=world.resolve_active_cover(region);
    double total=0.0;
    for (const ActiveCoverPart& part:parts)
        total+=fields.get(part.cell,field);
    const double tolerance=1.0e-12*std::max(1.0,total);
    if (amount>total+tolerance)
        throw std::runtime_error("resource source has insufficient stock");

    double remaining=std::min(amount,total);
    double remaining_stock=total;
    for (std::size_t i=0;i<parts.size();++i) {
        const CellId cell=parts[i].cell;
        const double stock=fields.get(cell,field);
        if (!(stock>0.0)) continue;
        const double debit=(i+1U==parts.size() || !(remaining_stock>0.0))
            ? std::min(stock,remaining)
            : std::min(stock,remaining*stock/remaining_stock);
        fields.set(cell,field,std::max(0.0,stock-debit));
        remaining-=debit;
        remaining_stock-=stock;
    }
    if (remaining>tolerance) {
        for (const ActiveCoverPart& part:parts) {
            if (!(remaining>tolerance)) break;
            const double stock=fields.get(part.cell,field);
            const double debit=std::min(stock,remaining);
            fields.set(part.cell,field,std::max(0.0,stock-debit));
            remaining-=debit;
        }
    }
    if (remaining>tolerance)
        throw std::runtime_error("resource debit failed to realize exact transfer");
}

double food_capacity_kg(const FieldStore& fields,CellId cell,const FieldRegistry& registry) {
    const double grass=fields.get(cell,require_field(registry,"ecology.grass_carbon_kg"));
    const double shrub=fields.get(cell,require_field(registry,"ecology.shrub_carbon_kg"));
    const double tree=fields.get(cell,require_field(registry,"ecology.tree_carbon_kg"));
    return std::max(
        0.0,
        (0.025*grass+0.008*shrub+0.002*tree)/
            kVegetationCarbonFractionOfDryMass
    );
}

double wood_capacity_kg(const FieldStore& fields,CellId cell,const FieldRegistry& registry) {
    const double shrub=fields.get(cell,require_field(registry,"ecology.shrub_carbon_kg"));
    const double tree=fields.get(cell,require_field(registry,"ecology.tree_carbon_kg"));
    return std::max(
        0.0,
        (0.02*shrub+0.12*tree)/
            kVegetationCarbonFractionOfDryMass
    );
}

class ResourceRenewalSystem final : public ISimSystem {
public:
    explicit ResourceRenewalSystem(const FieldRegistry& registry):registry_(registry) {
        food_=require_field(registry_,"resources.plant_food_kg");
        wood_=require_field(registry_,"resources.wood_kg");
    }

    [[nodiscard]] std::string_view id() const override {
        return "resources.renewal";
    }
    [[nodiscard]] Tick cadence_ticks() const override { return 24; }
    [[nodiscard]] std::vector<std::string> after() const override {
        return {"ecology.carbon_cycle"};
    }
    [[nodiscard]] SystemAccess access() const override {
        return {
            {
                "field:ecology.grass_carbon_kg",
                "field:ecology.shrub_carbon_kg",
                "field:ecology.tree_carbon_kg",
                "field:resources.plant_food_kg",
                "field:resources.wood_kg"
            },
            {
                "field:resources.plant_food_kg",
                "field:resources.wood_kg"
            }
        };
    }

    void step(SystemContext& ctx) override {
        auto& fields=ctx.world.stores().get<FieldStore>();
        const double food_fraction=1.0-std::exp(-ctx.dt_days/30.0);
        const double wood_fraction=1.0-std::exp(-ctx.dt_days/365.0);
        for (CellId cell:ctx.world.active_cells()) {
            const double food_target=food_capacity_kg(fields,cell,registry_);
            const double wood_target=wood_capacity_kg(fields,cell,registry_);
            const double food_current=std::min(fields.get(cell,food_),food_target);
            const double wood_current=std::min(fields.get(cell,wood_),wood_target);
            fields.set(
                cell,food_,
                food_current+(food_target-food_current)*food_fraction
            );
            fields.set(
                cell,wood_,
                wood_current+(wood_target-wood_current)*wood_fraction
            );
        }
    }
private:
    const FieldRegistry& registry_;
    FieldId food_{},wood_{};
};

} // namespace

// Hydrology keeps its reference reservoirs private. ResourceAccess is a narrow
// friend declared by HydrologyStore so gameplay can withdraw exact surface
// water without mutating the projected diagnostic field directly.
class ResourceAccess {
public:
    static double surface_water_m3(const HydrologyStore& store,CellId region) {
        return store.nodes_.at(store.node_index(region)).surface_m3;
    }

    static void take_surface_water_m3(HydrologyStore& store,CellId region,double amount) {
        if (!(amount>=0.0) || !std::isfinite(amount))
            throw std::invalid_argument("surface-water request must be finite and non-negative");
        auto& node=store.nodes_.at(store.node_index(region));
        const double tolerance=1.0e-12*std::max(1.0,node.surface_m3);
        if (amount>node.surface_m3+tolerance)
            throw std::runtime_error("surface-water source has insufficient stock");
        node.surface_m3=std::max(0.0,node.surface_m3-std::min(amount,node.surface_m3));
        store.rebuild_lakes();
    }
};

const ResourceDescriptor& resource_descriptor(ResourceKind kind) {
    return kResourceDescriptors.at(resource_index(kind));
}

std::optional<ResourceKind> resource_kind_from_key(std::string_view key) {
    for (const auto& descriptor:kResourceDescriptors)
        if (descriptor.key==key) return descriptor.kind;
    return std::nullopt;
}

double PlayerInventoryStore::amount(ResourceKind kind) const {
    return amounts_.at(resource_index(kind));
}

void PlayerInventoryStore::add(ResourceKind kind,double amount_to_add) {
    if (!std::isfinite(amount_to_add) || !(amount_to_add>=0.0))
        throw std::invalid_argument("inventory addition must be finite and non-negative");
    const auto index=resource_index(kind);
    const double updated=amounts_[index]+amount_to_add;
    if (!std::isfinite(updated) || updated>1.0e30)
        throw std::overflow_error("player inventory overflow");
    amounts_[index]=updated;
}

void PlayerInventoryStore::save(BinaryWriter& writer) const {
    writer.pod<std::uint32_t>(static_cast<std::uint32_t>(amounts_.size()));
    for (double amount_value:amounts_) writer.pod(amount_value);
}

void PlayerInventoryStore::load(BinaryReader& reader,std::uint32_t version) {
    if (version!=1) throw std::runtime_error("unsupported player inventory version");
    const auto count=reader.pod<std::uint32_t>();
    if (count!=amounts_.size())
        throw std::runtime_error("player inventory resource count mismatch");
    std::array<double,5> staged{};
    for (double& amount_value:staged) {
        amount_value=reader.pod<double>();
        if (!std::isfinite(amount_value) || amount_value<0.0 || amount_value>1.0e30)
            throw std::runtime_error("invalid player inventory amount");
    }
    amounts_=staged;
}

void ResourceModule::register_fields(FieldRegistry& registry) {
    for (std::string_view key:{
             "resources.plant_food_kg",
             "resources.wood_kg",
             "resources.stone_kg",
             "resources.metal_ore_kg"
         }) {
        registry.register_field({
            std::string(key),"kg",FieldSemantics::Extensive,0.0,0.0,1.0e30
        });
    }
}

void ResourceModule::register_stores(
    StateStoreRegistry& stores,
    const FieldRegistry&
) {
    stores.emplace<PlayerInventoryStore>();
}

void ResourceModule::register_systems(
    Scheduler& scheduler,
    const FieldRegistry& registry
) {
    scheduler.add(std::make_unique<ResourceRenewalSystem>(registry));
}

void ResourceModule::initialize(WorldState& world,const FieldRegistry& registry) {
    auto& fields=world.stores().get<FieldStore>();
    const auto food=require_field(registry,"resources.plant_food_kg");
    const auto wood=require_field(registry,"resources.wood_kg");
    const auto stone=require_field(registry,"resources.stone_kg");
    const auto ore=require_field(registry,"resources.metal_ore_kg");
    const auto land=require_field(registry,"geography.land_fraction");
    const auto regolith=require_field(registry,"geology.regolith_thickness_m");
    const auto collision=require_field(registry,"geology.collision_forcing");
    const auto rift=require_field(registry,"geology.rift_forcing");
    const auto volcanic=require_field(registry,"geology.volcanic_arc_forcing");

    for (CellId cell:world.active_cells()) {
        const double effective_area=
            world.topology().area_m2(cell)*fields.get(cell,land);
        fields.set(cell,food,food_capacity_kg(fields,cell,registry));
        fields.set(cell,wood,wood_capacity_kg(fields,cell,registry));

        const double regolith_depth=std::clamp(fields.get(cell,regolith),0.0,4.0);
        fields.set(
            cell,stone,
            effective_area*(0.10+0.40*regolith_depth)
        );

        const double tectonic_signal=std::clamp(
            0.45*fields.get(cell,collision)+
            0.35*fields.get(cell,rift)+
            0.20*fields.get(cell,volcanic),
            0.0,
            1.0
        );
        const double grade=deterministic_unit(
            world.seed(),fnv1a64("resources.metal_ore.grade"),0,cell.raw()
        );
        const double pocket=std::clamp((grade-0.68)/0.32,0.0,1.0);
        fields.set(
            cell,ore,
            effective_area*0.02*pocket*(0.20+0.80*tectonic_signal)
        );
    }
}

std::unique_ptr<Simulation> make_survival_simulation(
    std::uint64_t seed,
    SimulationConfig config
) {
    auto simulation=std::make_unique<Simulation>(seed,config);
    simulation->add_module(std::make_unique<GeographyModule>());
    simulation->add_module(std::make_unique<ClimateModule>());
    simulation->add_module(std::make_unique<MagicModule>());
    simulation->add_module(std::make_unique<HydrologyModule>());
    simulation->add_module(std::make_unique<EcologyModule>());
    simulation->add_module(std::make_unique<ResourceModule>());
    simulation->build();
    return simulation;
}

double resource_availability(
    const Simulation& simulation,
    Vec3d direction,
    ResourceKind kind
) {
    (void)resource_descriptor(kind);
    const CellId region=interaction_region(simulation,direction);
    const auto& world=simulation.world();
    const auto& fields=world.stores().get<FieldStore>();

    if (kind==ResourceKind::FreshWater) {
        const auto& hydrology=world.stores().get<HydrologyStore>();
        const double surface_m3=ResourceAccess::surface_water_m3(hydrology,region);
        const auto groundwater=require_field(
            simulation.fields(),"hydrology.groundwater_m3"
        );
        const double groundwater_m3=sum_extensive_region(
            world,fields,groundwater,region
        );
        return (surface_m3+groundwater_m3)*1000.0;
    }

    const auto field_id=require_field(
        simulation.fields(),stock_field_key(kind)
    );
    return sum_extensive_region(world,fields,field_id,region);
}

double collect_resource(
    Simulation& simulation,
    Vec3d direction,
    ResourceKind kind,
    double requested_amount
) {
    (void)resource_descriptor(kind);
    if (!std::isfinite(requested_amount) || !(requested_amount>0.0))
        throw std::invalid_argument("resource request must be finite and positive");

    const CellId region=interaction_region(simulation,direction);
    auto& world=simulation.world();
    auto& fields=world.stores().get<FieldStore>();
    auto& inventory=world.stores().get<PlayerInventoryStore>();
    const double current_inventory=inventory.amount(kind);
    if (!std::isfinite(current_inventory+requested_amount) ||
        current_inventory+requested_amount>1.0e30) {
        throw std::overflow_error("player inventory overflow");
    }

    const double available=resource_availability(simulation,direction,kind);
    if (requested_amount>available)
        throw std::runtime_error("requested resource amount exceeds local availability");

    if (kind==ResourceKind::FreshWater) {
        const double requested_m3=requested_amount/1000.0;
        auto& hydrology=world.stores().get<HydrologyStore>();
        const double surface_m3=ResourceAccess::surface_water_m3(hydrology,region);
        const double from_surface=std::min(surface_m3,requested_m3);
        const double from_groundwater=requested_m3-from_surface;
        if (from_groundwater>0.0) {
            const auto groundwater=require_field(
                simulation.fields(),"hydrology.groundwater_m3"
            );
            debit_extensive_region(
                world,fields,groundwater,region,from_groundwater
            );
        }
        if (from_surface>0.0)
            ResourceAccess::take_surface_water_m3(
                hydrology,region,from_surface
            );
        hydrology.project(world,simulation.fields());
    } else {
        const auto field_id=require_field(
            simulation.fields(),stock_field_key(kind)
        );
        debit_extensive_region(
            world,fields,field_id,region,requested_amount
        );
    }

    inventory.add(kind,requested_amount);
    world.emit({
        world.tick(),
        "gameplay.resource_collected",
        region,
        static_cast<std::uint64_t>(kind),
        requested_amount
    });
    return requested_amount;
}

double player_inventory_amount(
    const Simulation& simulation,
    ResourceKind kind
) {
    (void)resource_descriptor(kind);
    return simulation.world().stores().get<PlayerInventoryStore>().amount(kind);
}

} // namespace worldsim
