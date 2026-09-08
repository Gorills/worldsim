# Adding simulation domains

A domain is added through `ISimModule`; do not add domain flags to `Simulation`.

## Minimal domain

```cpp
class RadiationSystem final : public worldsim::ISimSystem {
public:
    explicit RadiationSystem(worldsim::FieldId dose) : dose_(dose) {}

    std::string_view id() const override { return "radiation.decay"; }
    worldsim::SystemAccess access() const override {
        return {{}, {"radiation.energy_j"}};
    }
    void step(worldsim::SystemContext& ctx) override;
private:
    worldsim::FieldId dose_{};
};

class RadiationModule final : public worldsim::ISimModule {
public:
    std::string_view id() const override { return "radiation"; }
    void register_fields(worldsim::FieldRegistry& r) override {
        energy_ = r.register_field({
            "radiation.energy_j", "J",
            worldsim::FieldSemantics::Extensive,
            0.0, 0.0, 1.0e40
        });
    }
    void register_systems(worldsim::Scheduler& s, const worldsim::FieldRegistry&) override {
        s.add(std::make_unique<RadiationSystem>(energy_));
    }
private:
    worldsim::FieldId energy_{};
};
```

Then:

```cpp
worldsim::Simulation sim(seed, config);
sim.add_module(std::make_unique<worldsim::GeographyModule>());
sim.add_module(std::make_unique<RadiationModule>());
sim.build();
```

The existing test `test_module_extension_contract()` verifies this mechanism using a test-only domain and verifies snapshot round-trip without editing the kernel.

## When a field is sufficient

Use a field when state is naturally one scalar per active spatial leaf and its LOD aggregation can be defined by the field semantics.

Examples:

- temperature;
- soil moisture volume;
- mana density/energy;
- biomass stock;
- mineral stock;
- air pollutant mass.

## When to create an `IStateStore`

Create a store when state has identity, relationships, multiple correlated attributes, or custom LOD rules.

Examples:

- species cohorts;
- epidemic compartment vectors per demographic cohort;
- settlements;
- firms/markets;
- armies and fleets;
- states and diplomatic relations;
- road/trade/river graphs;
- ownership and legal claims.

A store must implement how its state changes when a spatial cell is refined/coarsened. That is the point where the domain explicitly defines what "lower fidelity" means.

## Cross-domain coupling

Couple domains through named resources and explicit scheduler ordering. Do not call private implementation methods of another module.

Example:

```text
magic.flux writes      magic.temperature_anomaly_k
climate.surface reads  magic.temperature_anomaly_k (when present)
climate.surface writes climate.surface_temperature_k
vegetation.growth reads climate.surface_temperature_k
vegetation.growth reads magic.growth_factor
```

If a system both conflicts and lacks an ordering path, scheduler finalization fails.

## State migration rule

Changing a field key/unit/semantic changes the field schema and intentionally makes old snapshots incompatible. For production saves, introduce an explicit migration tool/version rather than weakening schema validation.
