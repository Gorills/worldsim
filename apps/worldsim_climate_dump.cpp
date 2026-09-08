#include "worldsim/climate.hpp"
#include "worldsim/hydrology.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

using namespace worldsim;
namespace {

std::ofstream output(const std::filesystem::path& path) {
    std::ofstream file(path);
    if (!file) throw std::runtime_error(
        "cannot open climate output: "+path.string()
    );
    file.exceptions(std::ios::badbit|std::ios::failbit);
    file<<std::setprecision(17);
    return file;
}

double sum_field(const Simulation& simulation, std::string_view key) {
    const auto id=simulation.fields().find(key);
    if (!id) throw std::runtime_error("missing diagnostic field");
    double sum=0.0;
    for (double value:simulation.world().stores()
        .get<FieldStore>().column(*id)) sum+=value;
    return sum;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t seed=42;
        unsigned days=730;
        unsigned level=2;
        bool adaptive=false;
        std::filesystem::path directory="out/climate";
        for (int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if (argument=="--adaptive") {
                adaptive=true;
                continue;
            }
            if (argument=="--help") {
                std::cout
                    <<"worldsim_climate_dump [--seed N] [--days N] "
                    <<"[--level 0..5] [--adaptive] [--output DIR]\n";
                return 0;
            }
            if (i+1>=argc)
                throw std::invalid_argument("missing argument value");
            const std::string value=argv[++i];
            if (argument=="--seed") seed=std::stoull(value);
            else if (argument=="--days")
                days=static_cast<unsigned>(std::stoul(value));
            else if (argument=="--level")
                level=static_cast<unsigned>(std::stoul(value));
            else if (argument=="--output") directory=value;
            else throw std::invalid_argument("unknown argument: "+argument);
        }
        if (days>36'500U || level>5U)
            throw std::invalid_argument(
                "diagnostic range: days <= 36500, level <= 5"
            );
        std::filesystem::create_directories(directory);
        auto simulation=make_default_simulation(
            seed,
            {
                static_cast<std::uint8_t>(level),
                static_cast<std::uint8_t>(level+(adaptive ? 1U : 0U)),
                3'600.0
            }
        );
        const ClimateStore& initial_climate=
            simulation->world().stores().get<ClimateStore>();
        const double initial_water=total_planet_water_m3(
            simulation->world(),simulation->fields()
        );
        const double water_scale=
            initial_climate.total_atmospheric_water_m3()+
            total_land_water_m3(
                simulation->world(),simulation->fields()
            );
        const double initial_heat=initial_climate.total_surface_heat_j();
        double maximum_water_residual=0.0;
        double maximum_energy_residual=0.0;

        auto history=output(directory/"history.csv");
        history
            <<"day,atmospheric_water_m3,ocean_water_m3,land_water_m3,"
            <<"planet_water_residual_m3,surface_heat_j,"
            <<"energy_residual_j,absorbed_solar_j,outgoing_longwave_j,"
            <<"land_temperature_k,ocean_temperature_k,"
            <<"precipitation_m3_day,evaporation_m3_day,"
            <<"land_snow_cover_fraction,land_albedo\n";
        for (unsigned day=0;day<=days;++day) {
            const ClimateStore& climate=
                simulation->world().stores().get<ClimateStore>();
            const ClimateBudget& budget=climate.budget();
            const double water=total_planet_water_m3(
                simulation->world(),simulation->fields()
            );
            const double water_residual=water-initial_water;
            const double expected_heat=
                initial_heat+budget.absorbed_solar_j-
                budget.outgoing_longwave_j;
            const double energy_residual=
                climate.total_surface_heat_j()-expected_heat;
            maximum_water_residual=std::max(
                maximum_water_residual,
                std::abs(water_residual)/std::max(1.0,water_scale)
            );
            maximum_energy_residual=std::max(
                maximum_energy_residual,
                std::abs(energy_residual)/std::max(1.0,std::abs(expected_heat))
            );

            double land_temperature=0.0;
            double ocean_temperature=0.0;
            double land_area=0.0;
            double ocean_area=0.0;
            double precipitation=0.0;
            double evaporation=0.0;
            double snow_cover=0.0;
            double snow_albedo_area=0.0;
            for (const ClimateNode& node:climate.nodes()) {
                const double local_ocean=std::max(
                    0.0,node.area_m2-node.land_area_m2
                );
                land_temperature+=
                    node.land_temperature_k*node.land_area_m2;
                ocean_temperature+=node.ocean_temperature_k*local_ocean;
                land_area+=node.land_area_m2;
                ocean_area+=local_ocean;
                precipitation+=node.precipitation_m3_day;
                evaporation+=node.evaporation_m3_day;
                snow_cover+=
                    node.snow_cover_fraction*node.land_area_m2;
                snow_albedo_area+=
                    climate_land_albedo(node.snow_cover_fraction)*
                    node.land_area_m2;
            }
            history
                <<day<<','<<climate.total_atmospheric_water_m3()<<','
                <<climate.ocean_water_m3()<<','
                <<total_land_water_m3(
                    simulation->world(),simulation->fields()
                )<<','<<water_residual<<','
                <<climate.total_surface_heat_j()<<','<<energy_residual<<','
                <<budget.absorbed_solar_j<<','
                <<budget.outgoing_longwave_j<<','
                <<land_temperature/std::max(1.0,land_area)<<','
                <<ocean_temperature/std::max(1.0,ocean_area)<<','
                <<precipitation<<','<<evaporation<<','
                <<snow_cover/std::max(1.0,land_area)<<','
                <<snow_albedo_area/std::max(1.0,land_area)<<'\n';
            if (day==days) break;
            if (adaptive && day%90U==0U) {
                if ((day/90U)%2U==0U)
                    simulation->set_focus({1.0,0.3,0.2});
                else
                    simulation->clear_focus();
            }
            simulation->step(24);
        }

        auto map=output(directory/"map.csv");
        map
            <<"cell_id,longitude_deg,latitude_deg,elevation_m,land_fraction,"
            <<"land_temperature_k,ocean_temperature_k,relative_humidity,"
            <<"atmospheric_water_mm,precipitation_mm_day,"
            <<"evaporation_mm_day,wind_east_m_s,wind_north_m_s,"
            <<"solar_flux_w_m2,net_radiation_w_m2,"
            <<"snow_cover_fraction,land_albedo\n";
        const auto& final_climate=
            simulation->world().stores().get<ClimateStore>();
        for (const ClimateNode& node:final_climate.nodes()) {
            const auto [latitude,longitude]=
                simulation->world().topology().lat_lon_rad(node.cell);
            map
                <<node.cell.raw()<<','<<longitude*180.0/kPi<<','
                <<latitude*180.0/kPi<<','<<node.mean_elevation_m<<','
                <<node.land_area_m2/node.area_m2<<','
                <<node.land_temperature_k<<','<<node.ocean_temperature_k<<','
                <<node.relative_humidity<<','
                <<node.atmospheric_water_m3/node.area_m2*1'000.0<<','
                <<node.precipitation_m3_day/node.area_m2*1'000.0<<','
                <<node.evaporation_m3_day/node.area_m2*1'000.0<<','
                <<node.east_wind_m_s<<','<<node.north_wind_m_s<<','
                <<node.solar_flux_w_m2<<','<<node.net_radiation_w_m2<<','
                <<node.snow_cover_fraction<<','
                <<climate_land_albedo(node.snow_cover_fraction)<<'\n';
        }

        auto summary=output(directory/"summary.json");
        summary
            <<"{\n  \"seed\": "<<seed
            <<",\n  \"days\": "<<days
            <<",\n  \"reference_level\": "<<level
            <<",\n  \"adaptive\": "<<(adaptive ? "true" : "false")
            <<",\n  \"maximum_relative_planet_water_residual\": "
            <<maximum_water_residual
            <<",\n  \"maximum_relative_surface_energy_residual\": "
            <<maximum_energy_residual
            <<",\n  \"water_residual_normalization_m3\": "
            <<water_scale
            <<",\n  \"final_atmospheric_water_m3\": "
            <<final_climate.total_atmospheric_water_m3()
            <<",\n  \"final_ocean_water_m3\": "
            <<final_climate.ocean_water_m3()
            <<",\n  \"final_land_water_m3\": "
            <<sum_field(*simulation,"hydrology.snow_water_m3")+
                sum_field(*simulation,"hydrology.soil_water_m3")+
                sum_field(*simulation,"hydrology.groundwater_m3")+
                simulation->world().stores()
                    .get<HydrologyStore>().total_surface_m3()
            <<",\n  \"final_land_area_weighted_snow_cover\": "
            <<[&] {
                double covered=0.0;
                double land_area=0.0;
                for (const ClimateNode& node:final_climate.nodes()) {
                    covered+=
                        node.snow_cover_fraction*node.land_area_m2;
                    land_area+=node.land_area_m2;
                }
                return covered/std::max(1.0,land_area);
            }()
            <<"\n}\n";
        std::cout
            <<"Climate: "<<days<<" days, "
            <<final_climate.nodes().size()<<" reference nodes; maximum water "
            <<"residual "<<maximum_water_residual<<", energy residual "
            <<maximum_energy_residual<<"; output "<<directory<<'\n';
        if (
            !std::isfinite(maximum_water_residual) ||
            !std::isfinite(maximum_energy_residual) ||
            maximum_water_residual>1.0e-8 ||
            maximum_energy_residual>1.0e-8
        ) return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
