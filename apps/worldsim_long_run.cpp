#include "worldsim/climate.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace worldsim;

namespace {

struct Options {
    std::uint64_t seed{42};
    unsigned years{100};
    unsigned level{2};
    bool fire{true};
    bool fauna{true};
    bool assert_stable{};
    std::optional<std::filesystem::path> output;
};

struct Component {
    double land_area{};
    double initial_vegetation{};
    double previous_burned{};
    std::vector<CellId> cells;
};

struct FieldIds {
    FieldId land,temperature,precipitation,fertility;
    FieldId vegetation,grass,shrub,tree,litter,fast_soil,slow_soil,npp;
    FieldId active_fire_area,burned_area,fire_emissions,pyrogenic_carbon;
    FieldId fauna_respired;
};

FieldId require_field(const Simulation& simulation, const char* key) {
    const auto id=simulation.fields().find(key);
    if (!id)
        throw std::runtime_error(std::string("missing field: ")+key);
    return *id;
}

double sum_field(const FieldStore& fields, FieldId id) {
    double total=0.0;
    for (double value:fields.column(id)) total+=value;
    return total;
}

std::string mode_name(const Options& options) {
    if (options.fire && options.fauna) return "coupled";
    if (options.fire) return "no-fauna";
    if (options.fauna) return "no-fire";
    return "no-fire-no-fauna";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index=1;index<argc;++index) {
        const std::string argument=argv[index];
        if (argument=="--no-fire") {
            options.fire=false;
            continue;
        }
        if (argument=="--no-fauna") {
            options.fauna=false;
            continue;
        }
        if (argument=="--assert-stable") {
            options.assert_stable=true;
            continue;
        }
        if (argument=="--mode") {
            if (++index>=argc)
                throw std::invalid_argument("--mode requires a value");
            const std::string mode=argv[index];
            if (mode=="coupled") {
                options.fire=true;
                options.fauna=true;
            } else if (mode=="no-fire") {
                options.fire=false;
                options.fauna=true;
            } else if (mode=="no-fauna") {
                options.fire=true;
                options.fauna=false;
            } else if (mode=="no-fire-no-fauna") {
                options.fire=false;
                options.fauna=false;
            } else {
                throw std::invalid_argument("unknown mode: "+mode);
            }
            continue;
        }
        if (++index>=argc)
            throw std::invalid_argument(argument+" requires a value");
        const std::string value=argv[index];
        if (argument=="--seed") options.seed=std::stoull(value);
        else if (argument=="--years")
            options.years=static_cast<unsigned>(std::stoul(value));
        else if (argument=="--level")
            options.level=static_cast<unsigned>(std::stoul(value));
        else if (argument=="--output") options.output=value;
        else throw std::invalid_argument("unknown argument: "+argument);
    }
    if (options.years==0U || options.years>1000U)
        throw std::invalid_argument("years must be in [1, 1000]");
    if (options.level>5U)
        throw std::invalid_argument("level must be in [0, 5]");
    return options;
}

std::unique_ptr<Simulation> make_experiment(const Options& options) {
    const auto level=static_cast<std::uint8_t>(options.level);
    auto simulation=std::make_unique<Simulation>(
        options.seed,
        SimulationConfig{level,level,3600.0}
    );
    simulation->add_module(std::make_unique<GeographyModule>());
    simulation->add_module(std::make_unique<ClimateModule>());
    simulation->add_module(std::make_unique<MagicModule>());
    simulation->add_module(std::make_unique<HydrologyModule>());
    simulation->add_module(std::make_unique<EcologyModule>(EcologyConfig{
        options.fire,
        options.fauna
    }));
    simulation->build();
    return simulation;
}

struct FaunaTotals {
    double herbivore_count{};
    double carnivore_count{};
    double carbon{};
};

FaunaTotals fauna_totals(const CohortStore& cohorts) {
    FaunaTotals totals;
    for (const auto& [id,cohort]:cohorts.all()) {
        (void)id;
        if (cohort.functional_group==1)
            totals.herbivore_count+=cohort.count;
        else if (cohort.functional_group==2)
            totals.carnivore_count+=cohort.count;
        totals.carbon+=cohort_carbon_kg(cohort);
    }
    return totals;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options=parse_options(argc,argv);
        auto simulation=make_experiment(options);
        auto& world=simulation->world();
        auto& fields=world.stores().get<FieldStore>();
        const FieldIds field{
            require_field(*simulation,"geography.land_fraction"),
            require_field(*simulation,"climate.surface_temperature_k"),
            require_field(*simulation,"climate.precipitation_mm_day"),
            require_field(*simulation,"ecology.soil_fertility"),
            require_field(*simulation,"ecology.vegetation_carbon_kg"),
            require_field(*simulation,"ecology.grass_carbon_kg"),
            require_field(*simulation,"ecology.shrub_carbon_kg"),
            require_field(*simulation,"ecology.tree_carbon_kg"),
            require_field(*simulation,"ecology.litter_carbon_kg"),
            require_field(*simulation,"ecology.soil_fast_carbon_kg"),
            require_field(*simulation,"ecology.soil_slow_carbon_kg"),
            require_field(*simulation,"ecology.npp_kg_day"),
            require_field(*simulation,"ecology.fire_active_area_m2"),
            require_field(*simulation,"ecology.fire_burned_area_m2"),
            require_field(*simulation,"ecology.fire_emitted_carbon_kg"),
            require_field(*simulation,"ecology.pyrogenic_carbon_kg"),
            require_field(*simulation,"ecology.fauna_respired_carbon_kg")
        };

        std::ofstream output_file;
        std::ostream* output=&std::cout;
        if (options.output) {
            if (!options.output->parent_path().empty())
                std::filesystem::create_directories(
                    options.output->parent_path()
                );
            output_file.open(*options.output);
            if (!output_file)
                throw std::runtime_error(
                    "cannot open output: "+options.output->string()
                );
            output=&output_file;
        }

        const std::vector<CellId> initial_cells(
            world.active_cells().begin(),world.active_cells().end()
        );
        std::vector<double> initial_density(initial_cells.size());
        double initial_vegetation=0.0;
        double baseline_vegetated_area=0.0;
        for (std::size_t index=0;index<initial_cells.size();++index) {
            const CellId cell=initial_cells[index];
            const double area=
                world.topology().area_m2(cell)*
                fields.get(cell,field.land);
            initial_density[index]=area>1.0
                ? fields.get(cell,field.vegetation)/area
                : 0.0;
            initial_vegetation+=fields.get(cell,field.vegetation);
            if (initial_density[index]>0.05)
                baseline_vegetated_area+=area;
        }

        std::vector<Component> components;
        std::set<CellId> unseen;
        for (CellId cell:world.active_cells())
            if (fields.get(cell,field.land)>0.2) unseen.insert(cell);
        while (!unseen.empty()) {
            Component component;
            std::queue<CellId> pending;
            pending.push(*unseen.begin());
            unseen.erase(unseen.begin());
            while (!pending.empty()) {
                const CellId cell=pending.front();
                pending.pop();
                component.cells.push_back(cell);
                component.land_area+=
                    world.topology().area_m2(cell)*
                    fields.get(cell,field.land);
                component.initial_vegetation+=
                    fields.get(cell,field.vegetation);
                component.previous_burned+=
                    fields.get(cell,field.burned_area);
                for (CellId neighbor:world.topology().neighbors4(cell)) {
                    const auto found=unseen.find(neighbor);
                    if (found!=unseen.end()) {
                        pending.push(neighbor);
                        unseen.erase(found);
                    }
                }
            }
            components.push_back(std::move(component));
        }

        const FaunaTotals initial_fauna=fauna_totals(
            world.stores().get<CohortStore>()
        );
        const double initial_water=total_planet_water_m3(
            world,simulation->fields()
        );
        const double initial_heat=
            world.stores().get<ClimateStore>().total_surface_heat_j();

        double previous_burned=sum_field(fields,field.burned_area);
        double temperature_sum=0.0;
        double precipitation_sum=0.0;
        double npp_sum=0.0;
        double peak_active_area=0.0;
        double minimum_vegetation=initial_vegetation;
        double maximum_vegetation=initial_vegetation;
        std::uint64_t samples=0;
        std::uint64_t ignitions=0;
        std::uint64_t near_extinctions=0;
        double final_vegetation_ratio=1.0;
        double final_collapsed_area_fraction=0.0;
        double final_worst_component_ratio=1.0;
        double final_fauna_carbon_ratio=1.0;
        unsigned final_invalid_values=0;

        *output << std::setprecision(10);
        *output
            << "mode,seed,level,year,days,cells,components,land_area_m2,veg_kgC,"
            << "veg_ratio_initial,veg_density_kg_m2,grass_share,shrub_share,tree_share,"
            << "litter_density_kg_m2,soil_density_kg_m2,mean_npp_PgC_yr,"
            << "annual_burn_land_fraction,cumulative_burn_land_turnovers,fire_emissions_PgC,"
            << "pyrogenic_PgC,peak_active_land_fraction,vegetated_area_fraction,"
            << "collapsed_baseline_area_fraction,worst_component_veg_ratio,"
            << "max_component_annual_burn_fraction,herbivore_ratio,carnivore_ratio,"
            << "fauna_carbon_ratio,fauna_carbon_PgC,fauna_respired_PgC,"
            << "mean_fertility,mean_land_temp_k,mean_land_precip_mm_day,"
            << "planet_water_rel_residual,surface_energy_rel_residual,"
            << "min_veg_ratio_year,max_veg_ratio_year,ignitions,near_extinctions,invalid_values\n";

        auto report=[&](unsigned year, unsigned days) {
            const double vegetation=sum_field(fields,field.vegetation);
            const double grass=sum_field(fields,field.grass);
            const double shrub=sum_field(fields,field.shrub);
            const double tree=sum_field(fields,field.tree);
            const double litter=sum_field(fields,field.litter);
            const double soil=
                sum_field(fields,field.fast_soil)+
                sum_field(fields,field.slow_soil);
            const double burned=sum_field(fields,field.burned_area);
            double current_land_area=0.0;
            double vegetated_area=0.0;
            double collapsed_area=0.0;
            double area_weighted_fertility=0.0;
            for (std::size_t index=0;index<initial_cells.size();++index) {
                const CellId cell=initial_cells[index];
                const double area=
                    world.topology().area_m2(cell)*
                    fields.get(cell,field.land);
                const double density=area>1.0
                    ? fields.get(cell,field.vegetation)/area
                    : 0.0;
                current_land_area+=area;
                area_weighted_fertility+=
                    fields.get(cell,field.fertility)*area;
                if (density>0.05) vegetated_area+=area;
                if (
                    initial_density[index]>0.05 &&
                    density<0.1*initial_density[index]
                ) {
                    collapsed_area+=area;
                }
            }

            double worst_component_ratio=
                std::numeric_limits<double>::infinity();
            double maximum_component_burn=0.0;
            for (Component& component:components) {
                double vegetation_now=0.0;
                double burned_now=0.0;
                for (CellId cell:component.cells) {
                    vegetation_now+=fields.get(cell,field.vegetation);
                    burned_now+=fields.get(cell,field.burned_area);
                }
                if (component.initial_vegetation>0.0)
                    worst_component_ratio=std::min(
                        worst_component_ratio,
                        vegetation_now/component.initial_vegetation
                    );
                maximum_component_burn=std::max(
                    maximum_component_burn,
                    (burned_now-component.previous_burned)/
                        std::max(1.0,component.land_area)
                );
                component.previous_burned=burned_now;
            }
            if (!std::isfinite(worst_component_ratio))
                worst_component_ratio=0.0;

            const FaunaTotals fauna=fauna_totals(
                world.stores().get<CohortStore>()
            );
            unsigned invalid_values=0;
            for (
                FieldId id=0;
                id<simulation->fields().size();
                ++id
            ) {
                const auto& descriptor=
                    simulation->fields().descriptor(id);
                for (double value:fields.column(id)) {
                    if (
                        !std::isfinite(value) ||
                        value<descriptor.min_value ||
                        value>descriptor.max_value
                    ) {
                        ++invalid_values;
                    }
                }
            }
            const auto& climate=world.stores().get<ClimateStore>();
            const auto& budget=climate.budget();
            const double water=total_planet_water_m3(
                world,simulation->fields()
            );
            const double expected_heat=
                initial_heat+
                budget.absorbed_solar_j-
                budget.outgoing_longwave_j;
            final_vegetation_ratio=
                vegetation/std::max(1.0,initial_vegetation);
            final_collapsed_area_fraction=
                collapsed_area/std::max(1.0,baseline_vegetated_area);
            final_worst_component_ratio=worst_component_ratio;
            final_fauna_carbon_ratio=
                fauna.carbon/std::max(1.0,initial_fauna.carbon);
            final_invalid_values=invalid_values;

            *output
                << mode_name(options) << ',' << options.seed << ','
                << options.level << ',' << year << ',' << days << ','
                << world.active_cells().size() << ','
                << components.size() << ',' << current_land_area << ','
                << vegetation << ','
                << vegetation/std::max(1.0,initial_vegetation) << ','
                << vegetation/std::max(1.0,current_land_area) << ','
                << grass/std::max(1.0,vegetation) << ','
                << shrub/std::max(1.0,vegetation) << ','
                << tree/std::max(1.0,vegetation) << ','
                << litter/std::max(1.0,current_land_area) << ','
                << soil/std::max(1.0,current_land_area) << ','
                << (samples
                    ? npp_sum/static_cast<double>(samples)*365.2422/1.0e12
                    : 0.0) << ','
                << (burned-previous_burned)/std::max(1.0,current_land_area) << ','
                << burned/std::max(1.0,current_land_area) << ','
                << sum_field(fields,field.fire_emissions)/1.0e12 << ','
                << sum_field(fields,field.pyrogenic_carbon)/1.0e12 << ','
                << peak_active_area/std::max(1.0,current_land_area) << ','
                << vegetated_area/std::max(1.0,current_land_area) << ','
                << collapsed_area/std::max(1.0,baseline_vegetated_area) << ','
                << worst_component_ratio << ',' << maximum_component_burn << ','
                << fauna.herbivore_count/
                    std::max(1.0,initial_fauna.herbivore_count) << ','
                << fauna.carnivore_count/
                    std::max(1.0,initial_fauna.carnivore_count) << ','
                << fauna.carbon/std::max(1.0,initial_fauna.carbon) << ','
                << fauna.carbon/1.0e12 << ','
                << sum_field(fields,field.fauna_respired)/1.0e12 << ','
                << area_weighted_fertility/
                    std::max(1.0,current_land_area) << ','
                << (samples
                    ? temperature_sum/static_cast<double>(samples)
                    : 0.0) << ','
                << (samples
                    ? precipitation_sum/static_cast<double>(samples)
                    : 0.0) << ','
                << (water-initial_water)/
                    std::max(1.0,std::abs(initial_water)) << ','
                << (climate.total_surface_heat_j()-expected_heat)/
                    std::max(1.0,std::abs(expected_heat)) << ','
                << minimum_vegetation/
                    std::max(1.0,initial_vegetation) << ','
                << maximum_vegetation/
                    std::max(1.0,initial_vegetation) << ','
                << ignitions << ',' << near_extinctions << ','
                << invalid_values << '\n' << std::flush;

            previous_burned=burned;
            temperature_sum=0.0;
            precipitation_sum=0.0;
            npp_sum=0.0;
            peak_active_area=0.0;
            minimum_vegetation=vegetation;
            maximum_vegetation=vegetation;
            samples=0;
            ignitions=0;
            near_extinctions=0;
        };

        report(0,0);
        unsigned elapsed_days=0;
        for (unsigned year=1;year<=options.years;++year) {
            const unsigned target_day=static_cast<unsigned>(
                std::llround(static_cast<double>(year)*365.2422)
            );
            while (elapsed_days<target_day) {
                simulation->step(24);
                ++elapsed_days;
                double temperature_area=0.0;
                double precipitation_area=0.0;
                double land_area=0.0;
                for (CellId cell:world.active_cells()) {
                    const double area=
                        world.topology().area_m2(cell)*
                        fields.get(cell,field.land);
                    land_area+=area;
                    temperature_area+=
                        fields.get(cell,field.temperature)*area;
                    precipitation_area+=
                        fields.get(cell,field.precipitation)*area;
                }
                temperature_sum+=temperature_area/
                    std::max(1.0,land_area);
                precipitation_sum+=precipitation_area/
                    std::max(1.0,land_area);
                npp_sum+=sum_field(fields,field.npp);
                peak_active_area=std::max(
                    peak_active_area,
                    sum_field(fields,field.active_fire_area)
                );
                const double vegetation=sum_field(
                    fields,field.vegetation
                );
                minimum_vegetation=std::min(
                    minimum_vegetation,vegetation
                );
                maximum_vegetation=std::max(
                    maximum_vegetation,vegetation
                );
                for (const auto& event:world.drain_events()) {
                    if (event.type=="ecology.fire_ignited") ++ignitions;
                    else if (event.type=="cohort.near_extinction")
                        ++near_extinctions;
                }
                ++samples;
            }
            report(year,elapsed_days);
        }
        if (options.assert_stable) {
            if (
                final_invalid_values!=0U ||
                final_vegetation_ratio<0.50 ||
                final_vegetation_ratio>4.0 ||
                final_collapsed_area_fraction>0.25 ||
                final_worst_component_ratio<0.10 ||
                (
                    options.fauna &&
                    (
                        final_fauna_carbon_ratio<0.01 ||
                        final_fauna_carbon_ratio>32.0
                    )
                ) ||
                (
                    !options.fauna &&
                    final_fauna_carbon_ratio!=0.0
                )
            ) {
                throw std::runtime_error(
                    "stability acceptance envelope violated"
                );
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "worldsim_long_run: " << error.what() << '\n';
        return 1;
    }
}
