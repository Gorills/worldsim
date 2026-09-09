#include "worldsim/planetary_nitrogen.hpp"

#include "worldsim/soil_nitrogen.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace worldsim {
namespace {

constexpr double atmospheric_n2_reference_density_kg_m2=0.50;
constexpr double ocean_dissolved_reference_density_kg_m2=0.05;
constexpr double maximum_fixation_kg_m2_day=2.0e-6;
constexpr double reactive_deposition_timescale_days=45.0;
constexpr double ocean_denitrification_timescale_days=30.0*365.2422;

FieldId required_field(const FieldRegistry& r, std::string_view key) {
    const auto id=r.find(key);
    if (!id)
        throw std::runtime_error(
            "planetary nitrogen required field missing: "+std::string(key)
        );
    return *id;
}

void require_nonnegative_finite(double value, std::string_view name) {
    if (!std::isfinite(value) || value<0.0 || value>1.0e30)
        throw std::runtime_error(
            "invalid planetary nitrogen "+std::string(name)
        );
}

double relaxed_fraction(double dt_days, double timescale_days) {
    return -std::expm1(-dt_days/timescale_days);
}

} // namespace

void NitrogenStore::initialize(
    const WorldState& world,
    const FieldRegistry& r
) {
    double total_area=0.0;
    double ocean_area=0.0;
    const auto& fields=world.stores().get<FieldStore>();
    const FieldId land=required_field(r,"geography.land_fraction");
    for (CellId cell:world.active_cells()) {
        const double area=world.topology().area_m2(cell);
        total_area+=area;
        ocean_area+=area*(1.0-fields.get(cell,land));
    }

    atmospheric_n2_kg_=
        total_area*atmospheric_n2_reference_density_kg_m2;
    atmospheric_reactive_nitrogen_kg_=0.0;
    ocean_dissolved_nitrogen_kg_=
        ocean_area*ocean_dissolved_reference_density_kg_m2;
    budget_={};
}

void NitrogenStore::advance(
    WorldState& world,
    const FieldRegistry& r,
    double terrestrial_leached_kg,
    double fire_emitted_kg,
    double dt_days
) {
    require_nonnegative_finite(
        terrestrial_leached_kg,"terrestrial leaching"
    );
    require_nonnegative_finite(fire_emitted_kg,"fire emission");
    if (!std::isfinite(dt_days) || dt_days<0.0)
        throw std::invalid_argument(
            "planetary nitrogen timestep must be finite and non-negative"
        );

    ocean_dissolved_nitrogen_kg_+=terrestrial_leached_kg;
    atmospheric_reactive_nitrogen_kg_+=fire_emitted_kg;
    budget_.terrestrial_leached_to_ocean_kg+=terrestrial_leached_kg;
    budget_.fire_emitted_to_atmosphere_kg+=fire_emitted_kg;

    if (!(dt_days>0.0)) return;

    const double denitrified=
        ocean_dissolved_nitrogen_kg_*
        relaxed_fraction(
            dt_days,ocean_denitrification_timescale_days
        );
    ocean_dissolved_nitrogen_kg_-=denitrified;
    atmospheric_n2_kg_+=denitrified;
    budget_.ocean_denitrified_to_atmosphere_kg+=denitrified;

    auto& fields=world.stores().get<FieldStore>();
    const FieldId land=required_field(r,"geography.land_fraction");
    const FieldId temperature=required_field(
        r,"climate.surface_temperature_k"
    );
    const FieldId fertility=required_field(
        r,"ecology.soil_fertility"
    );
    const FieldId mineral=required_field(
        r,"ecology.mineral_nitrogen_kg"
    );
    const FieldId fixation=required_field(
        r,"ecology.nitrogen_fixation_kg_day"
    );
    const FieldId deposition_field=required_field(
        r,"ecology.nitrogen_deposition_kg_day"
    );

    double total_land_area=0.0;
    double requested_fixation=0.0;
    std::vector<double> fixation_request;
    fixation_request.reserve(world.active_cells().size());
    for (CellId cell:world.active_cells()) {
        const double effective_area=
            world.topology().area_m2(cell)*
            fields.get(cell,land);
        total_land_area+=effective_area;
        const double temperature_factor=std::exp(
            -std::pow(
                (fields.get(cell,temperature)-290.0)/25.0,
                2.0
            )
        );
        const double current_fertility=
            effective_area>1.0
                ? mineral_nitrogen_fertility(
                    fields.get(cell,mineral)/effective_area
                )
                : 0.0;
        const double scarcity=1.0-current_fertility;
        const double request=
            maximum_fixation_kg_m2_day*
            effective_area*
            temperature_factor*
            scarcity*
            dt_days;
        fixation_request.push_back(request);
        requested_fixation+=request;
    }

    const double deposition=
        total_land_area>0.0
            ? atmospheric_reactive_nitrogen_kg_*
                relaxed_fraction(
                    dt_days,reactive_deposition_timescale_days
                )
            : 0.0;
    atmospheric_reactive_nitrogen_kg_-=deposition;
    budget_.reactive_deposited_kg+=deposition;

    const double fixation_total=std::min(
        requested_fixation,
        atmospheric_n2_kg_
    );
    const double fixation_scale=requested_fixation>0.0
        ? fixation_total/requested_fixation
        : 0.0;
    atmospheric_n2_kg_-=fixation_total;
    budget_.fixed_from_atmosphere_kg+=fixation_total;

    std::size_t index=0;
    for (CellId cell:world.active_cells()) {
        const double effective_area=
            world.topology().area_m2(cell)*
            fields.get(cell,land);
        const double deposited=
            total_land_area>0.0
                ? deposition*effective_area/total_land_area
                : 0.0;
        const double fixed=fixation_request[index++]*fixation_scale;
        fields.add(cell,mineral,deposited+fixed);
        fields.set(
            cell,fixation,
            (dt_days>0.0) ? fixed/dt_days : 0.0
        );
        fields.set(
            cell,deposition_field,
            (dt_days>0.0) ? deposited/dt_days : 0.0
        );
        fields.set(
            cell,fertility,
            effective_area>1.0
                ? mineral_nitrogen_fertility(
                    fields.get(cell,mineral)/effective_area
                )
                : 0.0
        );
    }

    require_nonnegative_finite(
        atmospheric_n2_kg_,"atmospheric N2"
    );
    require_nonnegative_finite(
        atmospheric_reactive_nitrogen_kg_,
        "atmospheric reactive nitrogen"
    );
    require_nonnegative_finite(
        ocean_dissolved_nitrogen_kg_,
        "ocean dissolved nitrogen"
    );
}

void NitrogenStore::save(BinaryWriter& writer) const {
    writer.pod(atmospheric_n2_kg_);
    writer.pod(atmospheric_reactive_nitrogen_kg_);
    writer.pod(ocean_dissolved_nitrogen_kg_);
    writer.pod(budget_.fixed_from_atmosphere_kg);
    writer.pod(budget_.reactive_deposited_kg);
    writer.pod(budget_.terrestrial_leached_to_ocean_kg);
    writer.pod(budget_.fire_emitted_to_atmosphere_kg);
    writer.pod(budget_.ocean_denitrified_to_atmosphere_kg);
}

void NitrogenStore::load(
    BinaryReader& reader,
    std::uint32_t version
) {
    if (version!=1)
        throw std::runtime_error(
            "unsupported planetary nitrogen snapshot version"
        );

    const double atmospheric_n2=reader.pod<double>();
    const double atmospheric_reactive=reader.pod<double>();
    const double ocean_dissolved=reader.pod<double>();
    NitrogenBudget budget{
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>()
    };
    require_nonnegative_finite(atmospheric_n2,"atmospheric N2");
    require_nonnegative_finite(
        atmospheric_reactive,"atmospheric reactive nitrogen"
    );
    require_nonnegative_finite(
        ocean_dissolved,"ocean dissolved nitrogen"
    );
    require_nonnegative_finite(
        budget.fixed_from_atmosphere_kg,"fixation budget"
    );
    require_nonnegative_finite(
        budget.reactive_deposited_kg,"deposition budget"
    );
    require_nonnegative_finite(
        budget.terrestrial_leached_to_ocean_kg,"leaching budget"
    );
    require_nonnegative_finite(
        budget.fire_emitted_to_atmosphere_kg,"fire budget"
    );
    require_nonnegative_finite(
        budget.ocean_denitrified_to_atmosphere_kg,
        "denitrification budget"
    );
    if (reader.remaining()!=0U)
        throw std::runtime_error(
            "trailing planetary nitrogen snapshot bytes"
        );

    atmospheric_n2_kg_=atmospheric_n2;
    atmospheric_reactive_nitrogen_kg_=atmospheric_reactive;
    ocean_dissolved_nitrogen_kg_=ocean_dissolved;
    budget_=budget;
}

double total_planet_nitrogen_kg(
    const WorldState& world,
    const FieldRegistry& r
) {
    const auto& store=world.stores().get<NitrogenStore>();
    double total=
        store.atmospheric_n2_kg()+
        store.atmospheric_reactive_nitrogen_kg()+
        store.ocean_dissolved_nitrogen_kg();

    const auto& fields=world.stores().get<FieldStore>();
    for (std::string_view key:{
             "ecology.litter_nitrogen_kg",
             "ecology.soil_fast_nitrogen_kg",
             "ecology.soil_slow_nitrogen_kg",
             "ecology.mineral_nitrogen_kg",
             "ecology.grass_nitrogen_kg",
             "ecology.shrub_nitrogen_kg",
             "ecology.tree_nitrogen_kg"
         }) {
        const auto id=r.find(key);
        if (!id) continue;
        total=std::accumulate(
            fields.column(*id).begin(),
            fields.column(*id).end(),
            total
        );
    }
    return total;
}

} // namespace worldsim
