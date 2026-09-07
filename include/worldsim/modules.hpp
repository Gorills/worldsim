#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

class GeographyModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "geography"; }
    void register_fields(FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
};

class ClimateModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "climate"; }
    void register_fields(FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
};

class MagicModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "magic"; }
    void register_fields(FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
};

class EcologyModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "ecology"; }
    void register_fields(FieldRegistry&) override;
    void register_stores(StateStoreRegistry&, const FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
};

} // namespace worldsim
