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
    if (!file) throw std::runtime_error("cannot open hydrology output: "+path.string());
    file.exceptions(std::ios::badbit|std::ios::failbit);
    file<<std::setprecision(17);
    return file;
}
double sum_field(const Simulation& sim,const char* key) {
    double sum=0;
    for (double value:sim.world().stores().get<FieldStore>().column(*sim.fields().find(key))) sum+=value;
    return sum;
}
}
int main(int argc,char** argv) {
    try {
        std::uint64_t seed=42;
        unsigned days=730,level=2;
        bool adaptive=false;
        std::filesystem::path directory="out/hydrology";
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--adaptive") { adaptive=true; continue; }
            if (arg=="--help") {
                std::cout<<"worldsim_hydrology_dump [--seed N] [--days N] [--level 0..5] [--adaptive] [--output DIR]\n";
                return 0;
            }
            if (i+1>=argc) throw std::invalid_argument("missing argument value");
            const std::string value=argv[++i];
            if (arg=="--seed") seed=std::stoull(value);
            else if (arg=="--days") days=static_cast<unsigned>(std::stoul(value));
            else if (arg=="--level") level=static_cast<unsigned>(std::stoul(value));
            else if (arg=="--output") directory=value;
            else throw std::invalid_argument("unknown argument: "+arg);
        }
        if (days>36500 || level>5) throw std::invalid_argument("diagnostic range: days <= 36500, level <= 5");
        std::filesystem::create_directories(directory);
        auto sim=make_default_simulation(seed,{static_cast<std::uint8_t>(level),static_cast<std::uint8_t>(level+(adaptive ? 1 : 0)),3600});
        const double initial=total_land_water_m3(sim->world(),sim->fields());
        const auto& hydro=sim->world().stores().get<HydrologyStore>();
        std::size_t monitor=0;
        double largest=-1;
        for (std::size_t i=0;i<hydro.nodes().size();++i)
            if (hydro.nodes()[i].bed_m>=0 && hydro.drainage_area_m2(i)>largest) {
                monitor=i; largest=hydro.drainage_area_m2(i);
            }
        const CellId monitor_cell=hydro.nodes()[monitor].cell;
        auto history=output(directory/"history.csv");
        history<<"day,snow_m3,soil_m3,groundwater_m3,surface_m3,precipitation_m3,evaporation_m3,ocean_export_m3,budget_residual_m3,basin_water_m3,reach_discharge_m3_day,reach_level_m,reach_spill_m,reach_temperature_k\n";
        double max_relative_error=0;
        for (unsigned day=0;day<=days;++day) {
            const auto& store=sim->world().stores().get<HydrologyStore>();
            const auto& budget=store.budget();
            const auto& fs=sim->world().stores().get<FieldStore>();
            const double total=total_land_water_m3(sim->world(),sim->fields());
            const double residual=initial+budget.precipitation_m3-budget.evaporation_m3-budget.ocean_export_m3-total;
            max_relative_error=std::max(max_relative_error,std::abs(residual)/std::max(1.0,initial+budget.precipitation_m3));
            double basin_water=0,temperature=0,discharge=0;
            for (std::size_t i=0;i<store.nodes().size();++i)
                if (store.basin_id(i)==store.basin_id(monitor)) basin_water+=store.nodes()[i].surface_m3;
            for (CellId c:sim->world().active_cells()) if (store.basin_id(store.node_index(c))==store.basin_id(monitor))
                for (const auto* key:{"hydrology.soil_water_m3","hydrology.groundwater_m3","hydrology.snow_water_m3"})
                    basin_water+=fs.get(c,*sim->fields().find(key));
            for (const auto& part:sim->world().resolve_active_cover(monitor_cell)) {
                temperature+=fs.get(part.cell,*sim->fields().find("climate.surface_temperature_k"))*part.weight;
                discharge+=fs.get(part.cell,*sim->fields().find("geology.drainage_discharge_m3_day"));
            }
            history<<day<<','<<sum_field(*sim,"hydrology.snow_water_m3")<<','<<sum_field(*sim,"hydrology.soil_water_m3")<<','
                <<sum_field(*sim,"hydrology.groundwater_m3")<<','<<store.total_surface_m3()<<','<<budget.precipitation_m3<<','
                <<budget.evaporation_m3<<','<<budget.ocean_export_m3<<','<<residual<<','<<basin_water<<','<<discharge<<','
                <<store.level_m(monitor)<<','<<store.spill_level_m(monitor)<<','<<temperature<<'\n';
            if (day==days) break;
            if (adaptive && day%90==0) {
                if ((day/90)%2==0) sim->set_focus(sim->world().topology().center_unit(monitor_cell));
                else sim->clear_focus();
            }
            sim->step(24);
        }
        auto nodes=output(directory/"nodes.csv");
        nodes<<"cell_id,longitude_deg,latitude_deg,bed_m,land_area_m2,surface_m3,depth_m,level_m,spill_m,discharge_m3_day,basin_id,lake_id\n";
        const auto& final=sim->world().stores().get<HydrologyStore>();
        for (std::size_t i=0;i<final.nodes().size();++i) {
            const auto& n=final.nodes()[i];
            const auto [lat,lon]=sim->world().topology().lat_lon_rad(n.cell);
            nodes<<n.cell.raw()<<','<<lon*180/kPi<<','<<lat*180/kPi<<','<<n.bed_m<<','<<n.land_area_m2<<','<<n.surface_m3<<','
                <<final.depth_m(i)<<','<<final.level_m(i)<<','<<final.spill_level_m(i)<<','<<n.discharge_m3_day<<','
                <<final.basin_id(i)<<','<<final.lake_id(i)<<'\n';
        }
        auto map=output(directory/"map.csv");
        map<<"longitude_deg,latitude_deg,bed_m,depth_m,discharge_m3_day,snow_depth_m,groundwater_depth_m,basin_id\n";
        const auto& fs=sim->world().stores().get<FieldStore>();
        for (int y=0;y<180;++y) for (int x=0;x<360;++x) {
            const double lat=(89.5-static_cast<double>(y))*kPi/180,lon=(-179.5+static_cast<double>(x))*kPi/180;
            const auto region=sim->world().topology().from_direction({std::cos(lat)*std::cos(lon),std::cos(lat)*std::sin(lon),std::sin(lat)},static_cast<std::uint8_t>(level));
            const auto i=final.node_index(region);
            double snow=0,ground=0;
            for (const auto& part:sim->world().resolve_active_cover(region)) {
                snow+=fs.get(part.cell,*sim->fields().find("hydrology.snow_water_m3"));
                ground+=fs.get(part.cell,*sim->fields().find("hydrology.groundwater_m3"));
            }
            const double area=std::max(1.0,final.nodes()[i].land_area_m2);
            map<<lon*180/kPi<<','<<lat*180/kPi<<','<<final.nodes()[i].bed_m<<','<<final.depth_m(i)<<','
                <<final.nodes()[i].discharge_m3_day<<','<<snow/area<<','<<ground/area<<','<<final.basin_id(i)<<'\n';
        }
        auto metadata=output(directory/"summary.json");
        metadata<<"{\n  \"seed\": "<<seed<<",\n  \"days\": "<<days<<",\n  \"reference_level\": "<<level
            <<",\n  \"adaptive\": "<<(adaptive ? "true" : "false")<<",\n  \"monitor_cell\": \""<<monitor_cell.raw()
            <<"\",\n  \"max_relative_water_budget_error\": "<<max_relative_error<<"\n}\n";
        std::cout<<"Hydrology: "<<days<<" days, "<<final.nodes().size()<<" reference nodes; maximum relative budget error "
            <<max_relative_error<<"; output "<<directory<<'\n';
        if (!std::isfinite(max_relative_error) || max_relative_error>1e-9) return 2;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
