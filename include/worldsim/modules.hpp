#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

class GeographyModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "geography"; }
    void register_fields(FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
    void on_spatial_cover_changed(WorldState&, const FieldRegistry&) override;
};

class ClimateModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "climate"; }
    void register_fields(FieldRegistry&) override;
    void register_stores(StateStoreRegistry&, const FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
    void on_spatial_cover_changed(WorldState&, const FieldRegistry&) override;
};

class MagicModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "magic"; }
    void register_fields(FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
};

struct EcologyConfig {
    bool enable_fire{true};
    bool enable_fauna{true};
};

double total_ecology_nitrogen_accounted_kg(
    const WorldState&,
    const FieldRegistry&
);

class EcologyModule final : public ISimModule {
public:
    explicit EcologyModule(EcologyConfig config={}): config_(config) {}
    [[nodiscard]] std::string_view id() const override { return "ecology"; }
    void register_fields(FieldRegistry&) override;
    void register_stores(StateStoreRegistry&, const FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
    void on_spatial_cover_changed(WorldState&, const FieldRegistry&) override;
private:
    EcologyConfig config_;
};

} // namespace worldsim
