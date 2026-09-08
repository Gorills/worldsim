#include "worldsim/hydrology.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

namespace worldsim {
namespace {
constexpr double channel_fraction=0.01;
constexpr double floodplain_relief_m=2.0;
constexpr double max_substep_days=0.0625;
FieldId field(const FieldRegistry& r, std::string_view key) {
    auto id=r.find(key);
    if (!id) throw std::runtime_error("hydrology requires field: "+std::string(key));
    return *id;
}
void nonnegative(double value) {
    if (!std::isfinite(value) || value<0.0 || value>1.0e30)
        throw std::invalid_argument("invalid hydrology stock or interval");
}
double storage_area(const HydrologyNode& n) {
    return std::max(1.0,n.land_area_m2);
}
double depth_volume(double area, double depth) {
    const double d=std::max(0.0,depth);
    if (d<floodplain_relief_m)
        return area*(channel_fraction*d+
            (1.0-channel_fraction)*d*d/(2.0*floodplain_relief_m));
    return area*(d-0.5*floodplain_relief_m*(1.0-channel_fraction));
}
double volume_depth(double area, double volume) {
    const double v=volume/area;
    const double threshold=0.5*floodplain_relief_m*(1.0+channel_fraction);
    if (v>=threshold) return v+0.5*floodplain_relief_m*(1.0-channel_fraction);
    // Rationalized quadratic root remains accurate for almost-dry channels.
    return 2.0*v/(channel_fraction+std::sqrt(channel_fraction*channel_fraction+
        2.0*(1.0-channel_fraction)*v/floodplain_relief_m));
}
}

double soil_water_capacity_depth_m(double regolith_thickness_m) {
    return 0.02+0.28*(-std::expm1(-std::max(0.0,regolith_thickness_m)/0.75));
}

void HydrologyStore::on_add_cell(CellId cell) {
    if (!has_level_) { reference_level_=cell.level(); has_level_=true; }
}
void HydrologyStore::initialize(const WorldState& world, const FieldRegistry& r) {
    const auto& fs=world.stores().get<FieldStore>();
    nodes_.clear();
    for (CellId cell:world.active_cells()) {
        if (cell.level()!=reference_level_)
            throw std::runtime_error("hydrology initialization requires uniform cover");
        nodes_.push_back({cell,fs.get(cell,field(r,"geography.elevation_m")),
            world.topology().area_m2(cell)*fs.get(cell,field(r,"geography.land_fraction")),0,0,0});
    }
    budget_={}; geology_days_=0;
    rebuild_graph();
}
std::size_t HydrologyStore::node_index(CellId cell) const {
    while (cell.level()>reference_level_) cell=cell.parent();
    const auto it=index_.find(cell);
    if (it==index_.end()) throw std::invalid_argument("cell outside hydrology reference cover");
    return it->second;
}
double HydrologyStore::total_surface_m3() const {
    double sum=0;
    for (const auto& n:nodes_) sum+=n.surface_m3;
    return sum;
}
double HydrologyStore::depth_m(std::size_t i) const {
    const auto& n=nodes_.at(i);
    return volume_depth(storage_area(n),n.surface_m3);
}
double HydrologyStore::level_m(std::size_t i) const { return nodes_.at(i).bed_m+depth_m(i); }
double HydrologyStore::wetted_fraction(std::size_t i) const {
    if (nodes_.at(i).surface_m3<=0.0) return 0;
    return std::min(1.0,channel_fraction+(1.0-channel_fraction)*depth_m(i)/floodplain_relief_m);
}
double HydrologyStore::volume_at_level(std::size_t i, double level) const {
    const auto& n=nodes_.at(i);
    return depth_volume(storage_area(n),level-n.bed_m);
}
std::size_t HydrologyStore::lake_id(std::size_t i) const { return lake_.at(i); }
void HydrologyStore::add_surface_water(CellId region, double volume) {
    nonnegative(volume);
    auto& n=nodes_.at(node_index(region));
    nonnegative(n.surface_m3+volume);
    n.surface_m3+=volume;
    rebuild_lakes();
}
double HydrologyStore::take_surface(std::size_t i, double request) {
    auto& n=nodes_[i];
    const double taken=std::min(n.surface_m3,std::max(0.0,request));
    n.surface_m3-=taken;
    return taken;
}
void HydrologyStore::set_bed_elevation(CellId region, double elevation) {
    if (region.level()!=reference_level_ || !std::isfinite(elevation) || std::abs(elevation)>1.0e6)
        throw std::invalid_argument("invalid hydrology bed elevation");
    nodes_.at(node_index(region)).bed_m=elevation;
    rebuild_drainage(); rebuild_lakes();
}
void HydrologyStore::apply_bed_changes(const WorldState& world, const std::map<CellId,double>& delta,
    const std::map<CellId,double>& land_delta) {
    std::vector<double> changes(nodes_.size());
    for (const auto& [cell,d]:delta) {
        const auto i=node_index(cell);
        changes[i]+=d*world.topology().area_m2(cell)/world.topology().area_m2(nodes_[i].cell);
    }
    for (std::size_t i=0;i<nodes_.size();++i) nodes_[i].bed_m+=changes[i];
    std::vector<double> area_change(nodes_.size());
    for (const auto& [cell,d]:land_delta) area_change[node_index(cell)]+=d*world.topology().area_m2(cell);
    for (std::size_t i=0;i<nodes_.size();++i)
        nodes_[i].land_area_m2=std::clamp(nodes_[i].land_area_m2+area_change[i],0.0,world.topology().area_m2(nodes_[i].cell));
    rebuild_drainage(); rebuild_lakes();
}
void HydrologyStore::rebuild_graph() {
    index_.clear(); links_.clear();
    neighbors_.assign(nodes_.size(),{});
    CubeSphereTopology topology;
    for (std::size_t i=0;i<nodes_.size();++i) index_.emplace(nodes_[i].cell,i);
    for (std::size_t i=0;i<nodes_.size();++i) {
        // This is the persistent UNIFORM reference cover, never active leaves.
        for (CellId neighbor:topology.neighbors4(nodes_[i].cell)) {
            const auto j=index_.at(neighbor);
            neighbors_[i].push_back(j);
            if (i>=j) continue;
            const double distance=kEarthRadiusM*std::acos(std::clamp(dot(
                topology.center_unit(nodes_[i].cell),topology.center_unit(neighbor)),-1.0,1.0));
            // Reduced reservoir travel time at nominal 1 m/s, not a solved velocity.
            links_.push_back({i,j,std::max(0.25,distance/86400.0)});
        }
        std::sort(neighbors_[i].begin(),neighbors_[i].end());
    }
    rebuild_drainage(); rebuild_lakes();
}
void HydrologyStore::rebuild_drainage() {
    const auto size=nodes_.size();
    spill_.assign(size,std::numeric_limits<double>::infinity());
    basin_.assign(size,0); drainage_area_.assign(size,0);
    std::vector<std::size_t> parent(size),order;
    using Entry=std::pair<double,std::size_t>;
    std::priority_queue<Entry,std::vector<Entry>,std::greater<>> queue;
    for (std::size_t i=0;i<size;++i) if (nodes_[i].bed_m<0) {
        spill_[i]=nodes_[i].bed_m; parent[i]=i; basin_[i]=i;
        queue.emplace(spill_[i],i);
    }
    if (queue.empty() && size>0) {
        const auto it=std::min_element(nodes_.begin(),nodes_.end(),[](const auto& a,const auto& b){return a.bed_m<b.bed_m;});
        const auto i=static_cast<std::size_t>(it-nodes_.begin());
        spill_[i]=nodes_[i].bed_m; parent[i]=i; basin_[i]=i;
        queue.emplace(spill_[i],i);
    }
    while (!queue.empty()) {
        const auto [height,i]=queue.top(); queue.pop();
        order.push_back(i);
        for (auto j:neighbors_[i]) {
            if (std::isfinite(spill_[j])) continue;
            spill_[j]=std::max(height,nodes_[j].bed_m);
            parent[j]=i; basin_[j]=basin_[i];
            queue.emplace(spill_[j],j);
        }
    }
    for (std::size_t i=0;i<size;++i) drainage_area_[i]=nodes_[i].land_area_m2;
    for (auto it=order.rbegin();it!=order.rend();++it)
        if (parent[*it]!=*it) drainage_area_[parent[*it]]+=drainage_area_[*it];
}
void HydrologyStore::rebuild_lakes() {
    lake_.assign(nodes_.size(),0);
    std::vector<std::size_t> root(nodes_.size());
    std::iota(root.begin(),root.end(),0);
    const auto find=[&](std::size_t i) {
        while (root[i]!=i) { root[i]=root[root[i]]; i=root[i]; }
        return i;
    };
    for (const auto& link:links_) {
        const auto a=link.a,b=link.b;
        const double sill=std::max(nodes_[a].bed_m,nodes_[b].bed_m);
        if (nodes_[a].bed_m<0 || nodes_[b].bed_m<0 ||
            nodes_[a].surface_m3<=0 || nodes_[b].surface_m3<=0) continue;
        if (std::min(level_m(a),level_m(b))>sill) {
            const auto ra=find(a),rb=find(b);
            root[std::max(ra,rb)]=std::min(ra,rb);
        }
    }
    for (std::size_t i=0;i<nodes_.size();++i)
        if (nodes_[i].bed_m>=0 && depth_m(i)>0.05 &&
            (spill_[i]>nodes_[i].bed_m || wetted_fraction(i)>0.5))
            lake_[i]=find(i)+1;
}
void HydrologyStore::route(double dt_days) {
    nonnegative(dt_days);
    if (dt_days>366.0) throw std::invalid_argument("hydrology interval exceeds 366 days");
    if (!(dt_days>0)) return;
    std::vector<double> sent(nodes_.size()),out(nodes_.size()),delta(nodes_.size());
    std::vector<double> transfers(links_.size());
    std::vector<std::size_t> donors(links_.size());
    // All-land sinks retain their water. Ocean nodes have no return boundary flux.
    for (auto& n:nodes_) if (n.bed_m<0) {
        budget_.ocean_export_m3+=n.surface_m3; n.surface_m3=0;
    }
    double remaining=dt_days;
    while (remaining>0) {
        const double dt=std::min(remaining,max_substep_days);
        std::fill(out.begin(),out.end(),0); std::fill(delta.begin(),delta.end(),0);
        for (std::size_t k=0;k<links_.size();++k) {
            const auto& link=links_[k];
            auto a=link.a,b=link.b;
            if (level_m(a)<level_m(b)) std::swap(a,b);
            donors[k]=a; transfers[k]=0;
            const auto& source=nodes_[a]; const auto& target=nodes_[b];
            const double head=level_m(a);
            if (source.bed_m<0 || source.surface_m3<=0 ||
                head<=std::max(target.bed_m,level_m(b))) continue;
            double equilibrium=source.surface_m3;
            if (target.bed_m>=0 && target.surface_m3+equilibrium>volume_at_level(b,source.bed_m)) {
                // Pair equilibrium respects both nonlinear storage curves. Most
                // downhill river links can empty the donor and skip this solve.
                double lo=0,hi=equilibrium;
                for (int iteration=0;iteration<28;++iteration) {
                    const double q=0.5*(lo+hi);
                    const double ha=source.bed_m+volume_depth(storage_area(source),source.surface_m3-q);
                    const double hb=target.bed_m+volume_depth(storage_area(target),target.surface_m3+q);
                    if (ha>hb) lo=q; else hi=q;
                }
                equilibrium=lo;
            }
            const double mobile=std::max(0.0,source.surface_m3-volume_at_level(a,target.bed_m));
            transfers[k]=std::min(mobile,equilibrium)*(-std::expm1(-dt/link.travel_days));
            out[a]+=transfers[k];
        }
        for (std::size_t k=0;k<links_.size();++k) {
            const auto a=donors[k];
            const auto b=a==links_[k].a ? links_[k].b : links_[k].a;
            const double scale=out[a]>nodes_[a].surface_m3 ? nodes_[a].surface_m3/out[a] : 1.0;
            const double q=transfers[k]*scale;
            delta[a]-=q; sent[a]+=q;
            if (nodes_[b].bed_m<0) budget_.ocean_export_m3+=q;
            else delta[b]+=q;
        }
        for (std::size_t i=0;i<nodes_.size();++i)
            nodes_[i].surface_m3=std::max(0.0,nodes_[i].surface_m3+delta[i]);
        remaining-=dt;
    }
    for (std::size_t i=0;i<nodes_.size();++i) {
        nodes_[i].discharge_m3_day=sent[i]/dt_days;
        nodes_[i].erosion_volume_m3+=sent[i];
    }
    geology_days_+=dt_days;
    rebuild_lakes();
}

void HydrologyStore::project(WorldState& world, const FieldRegistry& r) const {
    auto& fs=world.stores().get<FieldStore>();
    CubeSphereTopology topology;
    const auto surface=field(r,"hydrology.surface_water_m3");
    const auto depth=field(r,"hydrology.surface_depth_m");
    const auto level=field(r,"hydrology.surface_level_m");
    const auto wet=field(r,"hydrology.flooded_fraction");
    const auto discharge=field(r,"hydrology.river_discharge_m3_day");
    const auto basin=field(r,"hydrology.basin_id");
    const auto lake=field(r,"hydrology.lake_id");
    const auto spill=field(r,"hydrology.spill_elevation_m");
    for (CellId cell:world.active_cells()) {
        const auto i=node_index(cell); const auto& n=nodes_[i];
        const double weight=topology.area_m2(cell)/topology.area_m2(n.cell);
        fs.set(cell,surface,n.surface_m3*weight);
        fs.set(cell,depth,depth_m(i)); fs.set(cell,level,level_m(i));
        // The permanent channel strip itself is not terrestrial inundation.
        fs.set(cell,wet,n.bed_m<0 ? 0.0 : std::max(0.0,(wetted_fraction(i)-channel_fraction)/(1.0-channel_fraction)));
        fs.set(cell,discharge,n.discharge_m3_day*weight);
        fs.set(cell,basin,static_cast<double>(basin_id(i)));
        fs.set(cell,lake,static_cast<double>(lake_id(i)));
        fs.set(cell,spill,spill_[i]);
    }
}
void HydrologyStore::consume_geology_discharge(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    const auto discharge=field(r,"geology.drainage_discharge_m3_day");
    const auto area=field(r,"geology.drainage_area_m2");
    for (CellId cell:world.active_cells()) {
        const auto i=node_index(cell); const auto& n=nodes_[i];
        const double weight=world.topology().area_m2(cell)/world.topology().area_m2(n.cell);
        fs.set(cell,discharge,geology_days_>0 ? n.erosion_volume_m3/geology_days_*weight : 0.0);
        fs.set(cell,area,drainage_area_[i]*weight);
    }
    for (auto& n:nodes_) n.erosion_volume_m3=0;
    geology_days_=0;
}
void HydrologyStore::save(BinaryWriter& w) const {
    w.pod(reference_level_);
    w.pod(budget_.precipitation_m3); w.pod(budget_.evaporation_m3); w.pod(budget_.ocean_export_m3);
    w.pod(geology_days_); w.pod<std::uint64_t>(nodes_.size());
    for (const auto& n:nodes_) {
        w.pod(n.cell.raw()); w.pod(n.bed_m); w.pod(n.land_area_m2);
        w.pod(n.surface_m3); w.pod(n.discharge_m3_day); w.pod(n.erosion_volume_m3);
    }
}
void HydrologyStore::load(BinaryReader& r, std::uint32_t version) {
    if (version!=1) throw std::runtime_error("unsupported hydrology store version");
    const auto level=r.pod<std::uint8_t>();
    if (!has_level_ || level!=reference_level_) throw std::runtime_error("hydrology reference level mismatch");
    WaterBudget budget{r.pod<double>(),r.pod<double>(),r.pod<double>()};
    nonnegative(budget.precipitation_m3); nonnegative(budget.evaporation_m3); nonnegative(budget.ocean_export_m3);
    const double days=r.pod<double>(); nonnegative(days);
    const auto count=r.pod<std::uint64_t>();
    const auto expected=6ULL*(1ULL<<(2U*level));
    if (count!=expected || count>r.remaining()/48U)
        throw std::runtime_error("invalid hydrology node count");
    std::vector<HydrologyNode> nodes;
    CubeSphereTopology topology;
    for (std::uint64_t i=0;i<count;++i) {
        HydrologyNode n{CellId(r.pod<std::uint64_t>()),r.pod<double>(),r.pod<double>(),r.pod<double>(),r.pod<double>(),r.pod<double>()};
        if (!n.cell.valid() || n.cell.level()!=level ||
            (!nodes.empty() && !(nodes.back().cell<n.cell)) ||
            !std::isfinite(n.bed_m) || std::abs(n.bed_m)>1.0e6)
            throw std::runtime_error("invalid hydrology node geometry");
        nonnegative(n.land_area_m2); nonnegative(n.surface_m3);
        nonnegative(n.discharge_m3_day); nonnegative(n.erosion_volume_m3);
        if (n.land_area_m2>topology.area_m2(n.cell)*(1.0+1e-12))
            throw std::runtime_error("invalid hydrology land area");
        nodes.push_back(n);
    }
    nodes_=std::move(nodes); budget_=budget; geology_days_=days;
    rebuild_graph();
}
void HydrologyStore::validate_active_cover(const std::set<CellId>& cover) const {
    for (CellId cell:cover) (void)node_index(cell);
    if (nodes_.empty()) throw std::runtime_error("empty hydrology reference cover");
}

class HydrologySystem final : public ISimSystem {
public:
    explicit HydrologySystem(const FieldRegistry& r):r_(r) {}
    std::string_view id() const override { return "hydrology.balance"; }
    std::vector<std::string> after() const override { return {"climate.surface"}; }
    SystemAccess access() const override {
        SystemAccess a;
        for (const auto& key:{"geography.land_fraction","geology.regolith_thickness_m",
            "climate.surface_temperature_k","climate.precipitation_mm_day"}) a.reads.push_back("field:"+std::string(key));
        if (r_.find("ecology.vegetation_carbon_kg")) a.reads.push_back("field:ecology.vegetation_carbon_kg");
        for (std::size_t i=0;i<r_.size();++i) {
            const auto& key=r_.descriptor(static_cast<FieldId>(i)).key;
            if (key.starts_with("hydrology.")) { a.reads.push_back("field:"+key); a.writes.push_back("field:"+key); }
        }
        a.reads.push_back("store:hydrology.basins"); a.writes.push_back("store:hydrology.basins");
        return a;
    }
    void step(SystemContext& ctx) override {
        nonnegative(ctx.dt_days);
        if (ctx.dt_days>366.0) throw std::invalid_argument("hydrology interval exceeds 366 days");
        if (!(ctx.dt_days>0)) return;
        auto& fs=ctx.world.stores().get<FieldStore>();
        auto& store=ctx.world.stores().get<HydrologyStore>();
        const auto snow=field(r_,"hydrology.snow_water_m3"),soil=field(r_,"hydrology.soil_water_m3");
        const auto ground=field(r_,"hydrology.groundwater_m3"),runoff=field(r_,"hydrology.runoff_m3_day");
        const auto drainage=field(r_,"hydrology.drainage_since_soil_m3");
        const auto base=field(r_,"hydrology.baseflow_m3_day"),melt=field(r_,"hydrology.snowmelt_m3_day");
        const auto evap=field(r_,"hydrology.evapotranspiration_m3_day");
        const auto duration=field(r_,"hydrology.inundation_days");
        const auto temp=field(r_,"climate.surface_temperature_k"),precip=field(r_,"climate.precipitation_mm_day");
        const auto land=field(r_,"geography.land_fraction"),regolith=field(r_,"geology.regolith_thickness_m");
        const auto vegetation=r_.find("ecology.vegetation_carbon_kg");
        for (CellId cell:ctx.world.active_cells()) for (auto id:{runoff,base,melt,evap}) fs.set(cell,id,0);
        std::vector<double> routed(store.nodes_.size());
        double remaining=ctx.dt_days;
        while (remaining>0) {
            const double dt=std::min(remaining,max_substep_days);
            std::vector<double> surface_evap(store.nodes_.size()),surface_input(store.nodes_.size());
            std::vector<double> surface_before(store.nodes_.size()),wet_before(store.nodes_.size());
            for (std::size_t i=0;i<store.nodes_.size();++i) {
                surface_before[i]=store.nodes_[i].surface_m3;
                wet_before[i]=store.wetted_fraction(i);
            }
            for (CellId cell:ctx.world.active_cells()) {
                const auto i=store.node_index(cell);
                const double area=ctx.world.topology().area_m2(cell)*fs.get(cell,land);
                double s=fs.get(cell,snow),w=fs.get(cell,soil),g=fs.get(cell,ground);
                if (area<=1.0) {
                    store.budget_.ocean_export_m3+=s+w+g;
                    fs.set(cell,snow,0); fs.set(cell,soil,0); fs.set(cell,ground,0);
                    fs.set(cell,duration,0);
                    continue;
                }
                const double t=fs.get(cell,temp);
                const double p=fs.get(cell,precip)*0.001*area*dt;
                store.budget_.precipitation_m3+=p;
                const double rain_fraction=std::clamp((t-272.15)/2.0,0.0,1.0);
                s+=p*(1.0-rain_fraction);
                const double thaw=std::min(s,std::max(0.0,t-273.15)*0.003*area*dt);
                s-=thaw;
                const double input=p*rain_fraction+thaw;
                const double capacity=soil_water_capacity_depth_m(fs.get(cell,regolith))*area;
                const double conductivity=(0.002+0.025*(-std::expm1(-fs.get(cell,regolith)/0.75)))*area*dt;
                const double infiltrated=std::min({input,conductivity,std::max(0.0,capacity-w)});
                w+=infiltrated;
                double surface=input-infiltrated+std::max(0.0,w-capacity);
                w=std::min(w,capacity);
                const double recharge=std::max(0.0,w-0.65*capacity)*(-std::expm1(-dt/4.0));
                w-=recharge; g+=recharge;
                const double rise=std::min({g,std::max(0.0,0.45*capacity-w),0.001*area*dt});
                g-=rise; w+=rise;
                const double baseflow=g*(-std::expm1(-dt/45.0));
                g-=baseflow; surface+=baseflow;
                // Each adaptive leaf receives its own area share of pre-existing
                // reference surface water; order cannot favor the first child.
                const double node_area=ctx.world.topology().area_m2(store.nodes_[i].cell);
                const double share=ctx.world.topology().area_m2(cell)/node_area;
                const double flooded=std::max(0.0,(wet_before[i]-channel_fraction)/(1.0-channel_fraction));
                const double rewet=store.take_surface(i,std::min({
                    std::max(0.0,capacity-w),0.004*area*dt*flooded,surface_before[i]*share}));
                w+=rewet;
                const double biomass=vegetation ? fs.get(cell,*vegetation)/area : 0.0;
                const double canopy=-std::expm1(-biomass/2.0);
                const double pet=std::max(0.0,t-258.0)*0.000035*area*dt;
                const double demand=pet*(0.35+0.65*canopy)*(1.0-flooded);
                const double actual=std::min(w,demand);
                w-=actual; store.budget_.evaporation_m3+=actual;
                surface_evap[i]+=pet*wet_before[i];
                surface_input[i]+=surface;
                fs.set(cell,snow,s); fs.set(cell,soil,w); fs.set(cell,ground,g);
                fs.add(cell,runoff,surface/ctx.dt_days); fs.add(cell,base,baseflow/ctx.dt_days);
                fs.add(cell,melt,thaw/ctx.dt_days); fs.add(cell,evap,actual/ctx.dt_days);
                fs.add(cell,drainage,recharge+surface-baseflow);
                const double old_duration=fs.get(cell,duration);
                fs.set(cell,duration,flooded>0.1 ? old_duration+dt : old_duration*std::exp(-dt/2.0));
            }
            for (std::size_t i=0;i<store.nodes_.size();++i) {
                store.nodes_[i].surface_m3+=surface_input[i];
                store.budget_.evaporation_m3+=store.take_surface(i,surface_evap[i]);
            }
            store.route(dt);
            for (std::size_t i=0;i<routed.size();++i) routed[i]+=store.nodes_[i].discharge_m3_day*dt;
            remaining-=dt;
        }
        for (std::size_t i=0;i<routed.size();++i) store.nodes_[i].discharge_m3_day=routed[i]/ctx.dt_days;
        store.project(ctx.world,r_);
    }
private:
    const FieldRegistry& r_;
};

void HydrologyModule::register_fields(FieldRegistry& r) {
    for (const auto* key:{"soil_water_m3","snow_water_m3","groundwater_m3","surface_water_m3","drainage_since_soil_m3"})
        r.register_field({"hydrology."+std::string(key),"m3",FieldSemantics::Extensive,0,0,1e30});
    for (const auto* key:{"runoff_m3_day","river_discharge_m3_day","baseflow_m3_day","snowmelt_m3_day","evapotranspiration_m3_day"})
        r.register_field({"hydrology."+std::string(key),"m3/day",FieldSemantics::Extensive,0,0,1e30});
    r.register_field({"hydrology.surface_depth_m","m",FieldSemantics::Intensive,0,0,1e30});
    for (const auto* key:{"surface_level_m","spill_elevation_m"})
        r.register_field({"hydrology."+std::string(key),"m",FieldSemantics::Intensive,0,-1e6,1e30});
    r.register_field({"hydrology.flooded_fraction","1",FieldSemantics::Intensive,0,0,1});
    r.register_field({"hydrology.inundation_days","day",FieldSemantics::Intensive,0,0,1e30});
    for (const auto* key:{"basin_id","lake_id"})
        r.register_field({"hydrology."+std::string(key),"1",FieldSemantics::Intensive,0,0,1e15});
}
void HydrologyModule::register_stores(StateStoreRegistry& stores, const FieldRegistry&) { stores.emplace<HydrologyStore>(); }
void HydrologyModule::register_systems(Scheduler& scheduler, const FieldRegistry& r) { scheduler.add(std::make_unique<HydrologySystem>(r)); }
void HydrologyModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    for (CellId cell:world.active_cells()) {
        const double area=world.topology().area_m2(cell)*fs.get(cell,field(r,"geography.land_fraction"));
        fs.set(cell,field(r,"hydrology.soil_water_m3"),0.45*area*soil_water_capacity_depth_m(fs.get(cell,field(r,"geology.regolith_thickness_m"))));
        fs.set(cell,field(r,"hydrology.groundwater_m3"),0.1*area);
    }
    auto& store=world.stores().get<HydrologyStore>();
    store.initialize(world,r); store.project(world,r);
}
void HydrologyModule::on_spatial_cover_changed(WorldState& world, const FieldRegistry& r) { world.stores().get<HydrologyStore>().project(world,r); }
double total_land_water_m3(const WorldState& world, const FieldRegistry& r) {
    double total=world.stores().get<HydrologyStore>().total_surface_m3();
    const auto& fs=world.stores().get<FieldStore>();
    for (const auto* key:{"hydrology.snow_water_m3","hydrology.soil_water_m3","hydrology.groundwater_m3"})
        for (double value:fs.column(field(r,key))) total+=value;
    return total;
}

} // namespace worldsim
