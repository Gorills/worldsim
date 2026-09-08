#include "worldsim/spatial.hpp"
#include "worldsim/tectonics.hpp"
#include "worldsim/terrain.hpp"
#include "worldsim/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using worldsim::CellId;
using worldsim::CubeSphereTopology;
using worldsim::TectonicModel;
using worldsim::TectonicSample;
using worldsim::TerrainGenerator;
using worldsim::TerrainSample;
using worldsim::Vec3d;

constexpr double kGoldenAngleRad=2.3999632297286533222;
constexpr double kProbePhaseRad=0.917;
constexpr double kFourPi=4.0*worldsim::kPi;

struct Options {
    std::uint64_t seed_start=0;
    std::uint64_t seed_count=64;
    int samples=4096;
    std::uint8_t cover_level=5;
    std::filesystem::path output="out/geology-benchmark";
};

struct PlateMetrics {
    double area_cv{};
    double area_max_min_ratio{};
    double area_span_decades{};
    double normalized_area_entropy{};
    double center_nearest_neighbor_mean_deg{};
    double center_nearest_neighbor_cv{};
    int adjacency_pair_count{};
    double adjacency_degree_mean{};
    double adjacency_degree_cv{};
    double convergent_boundary_fraction{};
    double divergent_boundary_fraction{};
    double transform_boundary_fraction{};
};

struct CrustMetrics {
    double mean_affinity{};
    double high_affinity_area_fraction{};
    int high_affinity_component_count{};
    double largest_component_fraction_of_high_affinity{};
    double transition_edge_fraction{};
    double high_low_macro_relief_contrast_m{};
};

struct HypsometryMetrics {
    double land_fraction{};
    double mean_elevation_m{};
    double stddev_elevation_m{};
    double p05_m{};
    double p25_m{};
    double p50_m{};
    double p75_m{};
    double p95_m{};
    double deep_ocean_fraction{};
    double high_mountain_fraction{};
    double ocean_mode_m{};
    double land_mode_m{};
    double mode_separation_m{};
    double terrain_macro_correlation{};
};

struct CouplingMetrics {
    double uplift_active_fraction{};
    double divergence_active_fraction{};
    double uplift_macro_excess_m{};
    double oceanic_divergence_macro_excess_m{};
    double continental_divergence_macro_excess_m{};
};

struct SeedMetrics {
    std::uint64_t seed{};
    PlateMetrics plates;
    CrustMetrics crust;
    HypsometryMetrics hypsometry;
    CouplingMetrics coupling;
};

struct CoverSample {
    CellId cell;
    Vec3d direction;
    double area_m2{};
    std::uint32_t plate_id{};
    double crust_affinity{};
    double macro_elevation_m{};
};

struct ProbeSample {
    double elevation_m{};
    double macro_elevation_m{};
    double land_fraction{};
    double crust_affinity{};
    double uplift{};
    double divergence{};
};

struct RunningMoments {
    double sum{};
    double sum_sq{};
    std::size_t count{};

    void add(double value) {
        sum+=value;
        sum_sq+=value*value;
        ++count;
    }

    [[nodiscard]] double mean() const {
        return count==0 ? 0.0 : sum/static_cast<double>(count);
    }

    [[nodiscard]] double stddev() const {
        if (count==0) return 0.0;
        const double m=mean();
        return std::sqrt(std::max(0.0,sum_sq/static_cast<double>(count)-m*m));
    }
};

struct PairMoments {
    double x_sum{};
    double y_sum{};
    double xx_sum{};
    double yy_sum{};
    double xy_sum{};
    std::size_t count{};

    void add(double x, double y) {
        x_sum+=x;
        y_sum+=y;
        xx_sum+=x*x;
        yy_sum+=y*y;
        xy_sum+=x*y;
        ++count;
    }

    [[nodiscard]] double correlation() const {
        if (count<2) return 0.0;
        const double n=static_cast<double>(count);
        const double covariance=xy_sum-x_sum*y_sum/n;
        const double x_var=xx_sum-x_sum*x_sum/n;
        const double y_var=yy_sum-y_sum*y_sum/n;
        if (x_var<=0.0 || y_var<=0.0) return 0.0;
        return covariance/std::sqrt(x_var*y_var);
    }
};

struct GroupMean {
    double sum{};
    std::size_t count{};

    void add(double value) {
        sum+=value;
        ++count;
    }

    [[nodiscard]] double mean() const {
        return count==0 ? 0.0 : sum/static_cast<double>(count);
    }
};

[[nodiscard]] std::uint64_t parse_u64(std::string_view value, std::string_view name) {
    std::size_t consumed=0;
    const auto parsed=std::stoull(std::string(value),&consumed,10);
    if (consumed!=value.size()) throw std::invalid_argument(std::string(name)+" must be an unsigned integer");
    return parsed;
}

[[nodiscard]] int parse_int(std::string_view value, std::string_view name, int min_value, int max_value) {
    std::size_t consumed=0;
    const long parsed=std::stol(std::string(value),&consumed,10);
    if (consumed!=value.size() || parsed<min_value || parsed>max_value)
        throw std::invalid_argument(
            std::string(name)+" must be an integer in ["+
            std::to_string(min_value)+","+std::to_string(max_value)+"]"
        );
    return static_cast<int>(parsed);
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        auto require_value=[&]() -> std::string_view {
            if (i+1>=argc) throw std::invalid_argument(std::string(arg)+" requires a value");
            ++i;
            return argv[i];
        };
        if (arg=="--seed-start") {
            options.seed_start=parse_u64(require_value(),arg);
        } else if (arg=="--seed-count") {
            options.seed_count=parse_u64(require_value(),arg);
            if (options.seed_count==0 || options.seed_count>1024)
                throw std::invalid_argument("--seed-count must be in [1,1024]");
        } else if (arg=="--samples") {
            options.samples=parse_int(require_value(),arg,256,262144);
        } else if (arg=="--cover-level") {
            options.cover_level=static_cast<std::uint8_t>(
                parse_int(require_value(),arg,3,7)
            );
        } else if (arg=="--output") {
            options.output=std::filesystem::path(require_value());
        } else if (arg=="--quick") {
            options.seed_count=4;
            options.samples=1024;
            options.cover_level=4;
        } else if (arg=="--help" || arg=="-h") {
            std::cout
                << "Usage: worldsim_geology_benchmark [options]\n"
                << "  --seed-start N   first seed, default 0\n"
                << "  --seed-count N   number of consecutive seeds, default 64\n"
                << "  --samples N      equal-area probes per seed, default 4096\n"
                << "  --cover-level N  cube-sphere topology level 3..7, default 5\n"
                << "  --output DIR     output directory\n"
                << "  --quick          4 seeds / 1024 probes / level 4\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: "+std::string(arg));
        }
    }
    if (options.seed_start>std::numeric_limits<std::uint64_t>::max()-options.seed_count)
        throw std::invalid_argument("seed range overflows uint64");
    return options;
}

[[nodiscard]] Vec3d fibonacci_direction(int index, int count) {
    const double z=1.0-2.0*(static_cast<double>(index)+0.5)/static_cast<double>(count);
    const double phi=kProbePhaseRad+static_cast<double>(index)*kGoldenAngleRad;
    const double radial=std::sqrt(std::max(0.0,1.0-z*z));
    return {radial*std::cos(phi),radial*std::sin(phi),z};
}

[[nodiscard]] double angular_distance(Vec3d a, Vec3d b) {
    return std::acos(std::clamp(worldsim::dot(a,b),-1.0,1.0));
}

[[nodiscard]] double coefficient_of_variation(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const double mean=std::accumulate(values.begin(),values.end(),0.0)/
        static_cast<double>(values.size());
    if (mean==0.0) return 0.0;
    double sum_sq=0.0;
    for (double value:values) {
        const double delta=value-mean;
        sum_sq+=delta*delta;
    }
    return std::sqrt(sum_sq/static_cast<double>(values.size()))/std::abs(mean);
}

[[nodiscard]] double normalized_entropy(const std::vector<double>& fractions) {
    double entropy=0.0;
    for (double p:fractions) {
        if (p>0.0) entropy-=p*std::log(p);
    }
    return entropy/std::log(static_cast<double>(fractions.size()));
}

[[nodiscard]] double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(),values.end());
    const double position=q*static_cast<double>(values.size()-1);
    const auto lower=static_cast<std::size_t>(std::floor(position));
    const auto upper=static_cast<std::size_t>(std::ceil(position));
    const double t=position-static_cast<double>(lower);
    return values[lower]+(values[upper]-values[lower])*t;
}

[[nodiscard]] double histogram_mode(const std::vector<double>& values,
                                    double min_value,
                                    double max_value,
                                    double bin_width) {
    const int bins=static_cast<int>(std::ceil((max_value-min_value)/bin_width));
    std::vector<std::size_t> counts(static_cast<std::size_t>(bins),0);
    for (double value:values) {
        if (value<min_value || value>=max_value) continue;
        const int bin=static_cast<int>((value-min_value)/bin_width);
        if (bin>=0 && bin<bins) ++counts[static_cast<std::size_t>(bin)];
    }
    const auto it=std::max_element(counts.begin(),counts.end());
    if (it==counts.end() || *it==0) return 0.5*(min_value+max_value);
    const auto index=static_cast<int>(std::distance(counts.begin(),it));
    return min_value+(static_cast<double>(index)+0.5)*bin_width;
}

[[nodiscard]] std::vector<CoverSample> sample_cover(const TectonicModel& tectonics,
                                                    const CubeSphereTopology& topology,
                                                    std::uint8_t level) {
    const auto cover=worldsim::uniform_cover(level);
    std::vector<CoverSample> out;
    out.reserve(cover.size());
    for (CellId cell:cover) {
        const Vec3d direction=topology.center_unit(cell);
        const TectonicSample sample=tectonics.sample_direction(direction);
        out.push_back({
            cell,
            direction,
            topology.area_m2(cell),
            sample.plate_id,
            sample.continental_affinity,
            sample.macro_elevation_m
        });
    }
    return out;
}

[[nodiscard]] std::vector<ProbeSample> sample_probes(const TectonicModel& tectonics,
                                                     const TerrainGenerator& terrain,
                                                     int count) {
    std::vector<ProbeSample> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i=0;i<count;++i) {
        const Vec3d direction=fibonacci_direction(i,count);
        const TectonicSample tectonic=tectonics.sample_direction(direction);
        const TerrainSample terrain_sample=terrain.sample_direction(direction);
        out.push_back({
            terrain_sample.elevation_m,
            tectonic.macro_elevation_m,
            terrain_sample.land_fraction,
            tectonic.continental_affinity,
            tectonic.uplift_forcing,
            tectonic.divergence_forcing
        });
    }
    return out;
}

[[nodiscard]] PlateMetrics plate_metrics(const TectonicModel& tectonics,
                                         const CubeSphereTopology& topology,
                                         const std::vector<CoverSample>& cover) {
    std::array<double,TectonicModel::kPlateCount> area_m2{};
    double total_area_m2=0.0;
    std::unordered_map<std::uint64_t,std::size_t> index_by_cell;
    index_by_cell.reserve(cover.size()*2U);
    for (std::size_t i=0;i<cover.size();++i) {
        const CoverSample& sample=cover[i];
        if (sample.plate_id>=area_m2.size()) throw std::runtime_error("plate id out of range");
        area_m2[sample.plate_id]+=sample.area_m2;
        total_area_m2+=sample.area_m2;
        index_by_cell.emplace(sample.cell.raw(),i);
    }

    std::vector<double> area_fractions;
    area_fractions.reserve(area_m2.size());
    for (double area:area_m2) area_fractions.push_back(area/total_area_m2);
    const auto [min_it,max_it]=std::minmax_element(area_fractions.begin(),area_fractions.end());
    if (min_it==area_fractions.end() || *min_it<=0.0) throw std::runtime_error("plate area collapsed to zero");

    std::array<std::array<bool,TectonicModel::kPlateCount>,TectonicModel::kPlateCount> adjacency{};
    double convergent_weight=0.0;
    double divergent_weight=0.0;
    double transform_weight=0.0;

    for (const CoverSample& sample:cover) {
        const auto neighbors=topology.neighbors4(sample.cell);
        for (CellId neighbor:neighbors) {
            if (sample.cell.raw()>=neighbor.raw()) continue;
            const auto found=index_by_cell.find(neighbor.raw());
            if (found==index_by_cell.end()) throw std::runtime_error("uniform-cover neighbor missing");
            const CoverSample& other=cover[found->second];
            if (sample.plate_id==other.plate_id) continue;

            const std::uint32_t a=std::min(sample.plate_id,other.plate_id);
            const std::uint32_t b=std::max(sample.plate_id,other.plate_id);
            adjacency[a][b]=true;
            adjacency[b][a]=true;

            const Vec3d midpoint=worldsim::normalized(sample.direction+other.direction);
            const TectonicSample boundary=tectonics.sample_direction(midpoint);
            const double edge_weight=angular_distance(sample.direction,other.direction);
            const double convergence=boundary.convergence;
            const double shear=boundary.shear;
            if (std::abs(shear)>std::abs(convergence)) {
                transform_weight+=edge_weight;
            } else if (convergence>=0.0) {
                convergent_weight+=edge_weight;
            } else {
                divergent_weight+=edge_weight;
            }
        }
    }

    std::vector<double> degrees;
    degrees.reserve(TectonicModel::kPlateCount);
    int adjacency_pairs=0;
    for (std::uint32_t a=0;a<TectonicModel::kPlateCount;++a) {
        int degree=0;
        for (std::uint32_t b=0;b<TectonicModel::kPlateCount;++b) {
            if (!adjacency[a][b]) continue;
            ++degree;
            if (a<b) ++adjacency_pairs;
        }
        degrees.push_back(static_cast<double>(degree));
    }

    std::vector<double> nearest_center_distances;
    nearest_center_distances.reserve(TectonicModel::kPlateCount);
    for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i) {
        double nearest=worldsim::kPi;
        for (std::uint32_t j=0;j<TectonicModel::kPlateCount;++j) {
            if (i==j) continue;
            nearest=std::min(
                nearest,
                angular_distance(
                    tectonics.plates()[i].seed_direction,
                    tectonics.plates()[j].seed_direction
                )
            );
        }
        nearest_center_distances.push_back(nearest*180.0/worldsim::kPi);
    }

    const double boundary_total=convergent_weight+divergent_weight+transform_weight;
    if (boundary_total<=0.0) throw std::runtime_error("no plate-boundary transitions resolved");

    PlateMetrics result;
    result.area_cv=coefficient_of_variation(area_fractions);
    result.area_max_min_ratio=*max_it/ *min_it;
    result.area_span_decades=std::log10(result.area_max_min_ratio);
    result.normalized_area_entropy=normalized_entropy(area_fractions);
    result.center_nearest_neighbor_mean_deg=
        std::accumulate(nearest_center_distances.begin(),nearest_center_distances.end(),0.0)/
        static_cast<double>(nearest_center_distances.size());
    result.center_nearest_neighbor_cv=coefficient_of_variation(nearest_center_distances);
    result.adjacency_pair_count=adjacency_pairs;
    result.adjacency_degree_mean=
        std::accumulate(degrees.begin(),degrees.end(),0.0)/static_cast<double>(degrees.size());
    result.adjacency_degree_cv=coefficient_of_variation(degrees);
    result.convergent_boundary_fraction=convergent_weight/boundary_total;
    result.divergent_boundary_fraction=divergent_weight/boundary_total;
    result.transform_boundary_fraction=transform_weight/boundary_total;
    return result;
}

[[nodiscard]] CrustMetrics crust_metrics(const CubeSphereTopology& topology,
                                         const std::vector<CoverSample>& cover) {
    std::unordered_map<std::uint64_t,std::size_t> index_by_cell;
    index_by_cell.reserve(cover.size()*2U);
    double total_area=0.0;
    double affinity_area_sum=0.0;
    double high_area=0.0;
    double high_macro_sum=0.0;
    double low_macro_sum=0.0;
    double high_macro_area=0.0;
    double low_macro_area=0.0;
    for (std::size_t i=0;i<cover.size();++i) {
        index_by_cell.emplace(cover[i].cell.raw(),i);
        total_area+=cover[i].area_m2;
        affinity_area_sum+=cover[i].crust_affinity*cover[i].area_m2;
        if (cover[i].crust_affinity>=0.5) {
            high_area+=cover[i].area_m2;
            high_macro_sum+=cover[i].macro_elevation_m*cover[i].area_m2;
            high_macro_area+=cover[i].area_m2;
        } else {
            low_macro_sum+=cover[i].macro_elevation_m*cover[i].area_m2;
            low_macro_area+=cover[i].area_m2;
        }
    }

    std::vector<bool> visited(cover.size(),false);
    std::vector<double> component_areas;
    for (std::size_t start=0;start<cover.size();++start) {
        if (visited[start] || cover[start].crust_affinity<0.5) continue;
        double component_area=0.0;
        std::queue<std::size_t> pending;
        pending.push(start);
        visited[start]=true;
        while (!pending.empty()) {
            const std::size_t current=pending.front();
            pending.pop();
            component_area+=cover[current].area_m2;
            for (CellId neighbor:topology.neighbors4(cover[current].cell)) {
                const auto found=index_by_cell.find(neighbor.raw());
                if (found==index_by_cell.end()) throw std::runtime_error("uniform-cover neighbor missing");
                const std::size_t next=found->second;
                if (visited[next] || cover[next].crust_affinity<0.5) continue;
                visited[next]=true;
                pending.push(next);
            }
        }
        component_areas.push_back(component_area);
    }

    std::size_t transition_edges=0;
    std::size_t total_edges=0;
    for (const CoverSample& sample:cover) {
        for (CellId neighbor:topology.neighbors4(sample.cell)) {
            if (sample.cell.raw()>=neighbor.raw()) continue;
            const auto found=index_by_cell.find(neighbor.raw());
            if (found==index_by_cell.end()) throw std::runtime_error("uniform-cover neighbor missing");
            const bool high_a=sample.crust_affinity>=0.5;
            const bool high_b=cover[found->second].crust_affinity>=0.5;
            ++total_edges;
            if (high_a!=high_b) ++transition_edges;
        }
    }

    CrustMetrics result;
    result.mean_affinity=affinity_area_sum/total_area;
    result.high_affinity_area_fraction=high_area/total_area;
    result.high_affinity_component_count=static_cast<int>(component_areas.size());
    const double largest=component_areas.empty() ? 0.0 :
        *std::max_element(component_areas.begin(),component_areas.end());
    result.largest_component_fraction_of_high_affinity=
        high_area>0.0 ? largest/high_area : 0.0;
    result.transition_edge_fraction=total_edges>0 ?
        static_cast<double>(transition_edges)/static_cast<double>(total_edges) : 0.0;
    result.high_low_macro_relief_contrast_m=
        high_macro_area>0.0 && low_macro_area>0.0 ?
        high_macro_sum/high_macro_area-low_macro_sum/low_macro_area : 0.0;
    return result;
}

[[nodiscard]] HypsometryMetrics hypsometry_metrics(const std::vector<ProbeSample>& probes) {
    std::vector<double> elevations;
    elevations.reserve(probes.size());
    RunningMoments elevation_moments;
    PairMoments terrain_macro;
    double land_sum=0.0;
    std::size_t deep_count=0;
    std::size_t high_count=0;

    for (const ProbeSample& sample:probes) {
        elevations.push_back(sample.elevation_m);
        elevation_moments.add(sample.elevation_m);
        terrain_macro.add(sample.elevation_m,sample.macro_elevation_m);
        land_sum+=sample.land_fraction;
        if (sample.elevation_m<-3000.0) ++deep_count;
        if (sample.elevation_m>2000.0) ++high_count;
    }

    const double n=static_cast<double>(probes.size());
    const double ocean_mode=histogram_mode(elevations,-6000.0,-1000.0,250.0);
    const double land_mode=histogram_mode(elevations,-500.0,6500.0,250.0);

    HypsometryMetrics result;
    result.land_fraction=land_sum/n;
    result.mean_elevation_m=elevation_moments.mean();
    result.stddev_elevation_m=elevation_moments.stddev();
    result.p05_m=percentile(elevations,0.05);
    result.p25_m=percentile(elevations,0.25);
    result.p50_m=percentile(elevations,0.50);
    result.p75_m=percentile(elevations,0.75);
    result.p95_m=percentile(elevations,0.95);
    result.deep_ocean_fraction=static_cast<double>(deep_count)/n;
    result.high_mountain_fraction=static_cast<double>(high_count)/n;
    result.ocean_mode_m=ocean_mode;
    result.land_mode_m=land_mode;
    result.mode_separation_m=land_mode-ocean_mode;
    result.terrain_macro_correlation=terrain_macro.correlation();
    return result;
}

[[nodiscard]] double matched_uplift_excess(const std::vector<ProbeSample>& probes) {
    constexpr std::size_t kBins=10;
    std::array<GroupMean,kBins> active{};
    std::array<GroupMean,kBins> neutral{};
    for (const ProbeSample& sample:probes) {
        const auto bin=static_cast<std::size_t>(std::clamp(
            static_cast<int>(sample.crust_affinity*static_cast<double>(kBins)),
            0,
            static_cast<int>(kBins)-1
        ));
        if (sample.uplift>=0.10) active[bin].add(sample.macro_elevation_m);
        if (sample.uplift<0.01 && sample.divergence<0.01)
            neutral[bin].add(sample.macro_elevation_m);
    }

    double weighted_delta_sum=0.0;
    std::size_t weight=0;
    for (std::size_t bin=0;bin<kBins;++bin) {
        if (active[bin].count<4 || neutral[bin].count<4) continue;
        weighted_delta_sum+=
            (active[bin].mean()-neutral[bin].mean())*
            static_cast<double>(active[bin].count);
        weight+=active[bin].count;
    }
    return weight==0 ? 0.0 : weighted_delta_sum/static_cast<double>(weight);
}

[[nodiscard]] double divergence_excess(const std::vector<ProbeSample>& probes,
                                       double affinity_min,
                                       double affinity_max) {
    GroupMean active;
    GroupMean neutral;
    for (const ProbeSample& sample:probes) {
        if (sample.crust_affinity<affinity_min || sample.crust_affinity>=affinity_max) continue;
        if (sample.divergence>=0.10) active.add(sample.macro_elevation_m);
        if (sample.uplift<0.01 && sample.divergence<0.01)
            neutral.add(sample.macro_elevation_m);
    }
    if (active.count<4 || neutral.count<4) return 0.0;
    return active.mean()-neutral.mean();
}

[[nodiscard]] CouplingMetrics coupling_metrics(const std::vector<ProbeSample>& probes) {
    std::size_t uplift_active=0;
    std::size_t divergence_active=0;
    for (const ProbeSample& sample:probes) {
        if (sample.uplift>=0.05) ++uplift_active;
        if (sample.divergence>=0.05) ++divergence_active;
    }

    CouplingMetrics result;
    const double n=static_cast<double>(probes.size());
    result.uplift_active_fraction=static_cast<double>(uplift_active)/n;
    result.divergence_active_fraction=static_cast<double>(divergence_active)/n;
    result.uplift_macro_excess_m=matched_uplift_excess(probes);
    result.oceanic_divergence_macro_excess_m=divergence_excess(probes,0.0,0.30);
    result.continental_divergence_macro_excess_m=divergence_excess(probes,0.70,1.01);
    return result;
}

[[nodiscard]] SeedMetrics evaluate_seed(std::uint64_t seed, const Options& options) {
    const TectonicModel tectonics(seed);
    const TerrainGenerator terrain(seed);
    const CubeSphereTopology topology;
    const std::vector<CoverSample> cover=sample_cover(tectonics,topology,options.cover_level);
    const std::vector<ProbeSample> probes=sample_probes(tectonics,terrain,options.samples);

    SeedMetrics result;
    result.seed=seed;
    result.plates=plate_metrics(tectonics,topology,cover);
    result.crust=crust_metrics(topology,cover);
    result.hypsometry=hypsometry_metrics(probes);
    result.coupling=coupling_metrics(probes);
    return result;
}

template<class Fn>
[[nodiscard]] double mean_metric(const std::vector<SeedMetrics>& seeds, Fn fn) {
    double sum=0.0;
    for (const SeedMetrics& seed:seeds) sum+=fn(seed);
    return sum/static_cast<double>(seeds.size());
}

template<class Fn>
[[nodiscard]] std::pair<double,double> minmax_metric(const std::vector<SeedMetrics>& seeds, Fn fn) {
    double min_value=std::numeric_limits<double>::infinity();
    double max_value=-std::numeric_limits<double>::infinity();
    for (const SeedMetrics& seed:seeds) {
        const double value=fn(seed);
        min_value=std::min(min_value,value);
        max_value=std::max(max_value,value);
    }
    return {min_value,max_value};
}

[[nodiscard]] std::vector<std::string> findings(const std::vector<SeedMetrics>& seeds) {
    std::vector<std::string> out;
    const double plate_ratio=mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_max_min_ratio; });
    const double center_cv=mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.center_nearest_neighbor_cv; });
    const double convergent=mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.convergent_boundary_fraction; });
    const double divergent=mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.divergent_boundary_fraction; });
    const double transform=mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.transform_boundary_fraction; });
    const double land=mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.land_fraction; });
    const double mode_separation=mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.mode_separation_m; });
    const double uplift_excess=mean_metric(seeds,[](const SeedMetrics& s){ return s.coupling.uplift_macro_excess_m; });

    // These are deliberately broad diagnostic warnings, not claims that Earth is
    // the only valid target. PB2002 reports a power-law plate-area spectrum over
    // 0.002..1 sr; a 16-plate model with <2x largest/smallest area has no
    // meaningful area scale hierarchy at all.
    if (plate_ratio<2.0 || center_cv<0.10)
        out.emplace_back("WARN plate_geometry_too_regular: plate areas/center spacing have very low diversity for an Earth-like plate spectrum");
    if (convergent<0.05 || divergent<0.05 || transform<0.05)
        out.emplace_back("WARN boundary_kinematics_degenerate: one coarse convergent/divergent/transform class occupies <5% of resolved boundary length");
    if (land<0.15 || land>0.45)
        out.emplace_back("WARN earthlike_land_fraction_outside_broad_reference: above-sea area is outside 15..45%");
    if (mode_separation<2500.0)
        out.emplace_back("WARN hypsometry_not_strongly_bimodal: ocean/land histogram modes are separated by <2500 m");
    if (uplift_excess<=0.0)
        out.emplace_back("WARN convergence_relief_coupling_missing: uplift-active samples are not elevated relative to crust-matched inactive samples");
    if (out.empty()) out.emplace_back("INFO no_broad_plausibility_warnings");
    return out;
}

void write_seed_json(std::ostream& out, const SeedMetrics& s, bool trailing_comma) {
    out << "    {\n";
    out << "      \"seed\": " << s.seed << ",\n";
    out << "      \"plates\": {\n";
    out << "        \"area_cv\": " << s.plates.area_cv << ",\n";
    out << "        \"area_max_min_ratio\": " << s.plates.area_max_min_ratio << ",\n";
    out << "        \"area_span_decades\": " << s.plates.area_span_decades << ",\n";
    out << "        \"normalized_area_entropy\": " << s.plates.normalized_area_entropy << ",\n";
    out << "        \"center_nearest_neighbor_mean_deg\": " << s.plates.center_nearest_neighbor_mean_deg << ",\n";
    out << "        \"center_nearest_neighbor_cv\": " << s.plates.center_nearest_neighbor_cv << ",\n";
    out << "        \"adjacency_pair_count\": " << s.plates.adjacency_pair_count << ",\n";
    out << "        \"adjacency_degree_mean\": " << s.plates.adjacency_degree_mean << ",\n";
    out << "        \"adjacency_degree_cv\": " << s.plates.adjacency_degree_cv << ",\n";
    out << "        \"convergent_boundary_fraction\": " << s.plates.convergent_boundary_fraction << ",\n";
    out << "        \"divergent_boundary_fraction\": " << s.plates.divergent_boundary_fraction << ",\n";
    out << "        \"transform_boundary_fraction\": " << s.plates.transform_boundary_fraction << "\n";
    out << "      },\n";
    out << "      \"crust\": {\n";
    out << "        \"mean_affinity\": " << s.crust.mean_affinity << ",\n";
    out << "        \"high_affinity_area_fraction\": " << s.crust.high_affinity_area_fraction << ",\n";
    out << "        \"high_affinity_component_count\": " << s.crust.high_affinity_component_count << ",\n";
    out << "        \"largest_component_fraction_of_high_affinity\": " << s.crust.largest_component_fraction_of_high_affinity << ",\n";
    out << "        \"transition_edge_fraction\": " << s.crust.transition_edge_fraction << ",\n";
    out << "        \"high_low_macro_relief_contrast_m\": " << s.crust.high_low_macro_relief_contrast_m << "\n";
    out << "      },\n";
    out << "      \"hypsometry\": {\n";
    out << "        \"land_fraction\": " << s.hypsometry.land_fraction << ",\n";
    out << "        \"mean_elevation_m\": " << s.hypsometry.mean_elevation_m << ",\n";
    out << "        \"stddev_elevation_m\": " << s.hypsometry.stddev_elevation_m << ",\n";
    out << "        \"p05_m\": " << s.hypsometry.p05_m << ",\n";
    out << "        \"p25_m\": " << s.hypsometry.p25_m << ",\n";
    out << "        \"p50_m\": " << s.hypsometry.p50_m << ",\n";
    out << "        \"p75_m\": " << s.hypsometry.p75_m << ",\n";
    out << "        \"p95_m\": " << s.hypsometry.p95_m << ",\n";
    out << "        \"deep_ocean_fraction\": " << s.hypsometry.deep_ocean_fraction << ",\n";
    out << "        \"high_mountain_fraction\": " << s.hypsometry.high_mountain_fraction << ",\n";
    out << "        \"ocean_mode_m\": " << s.hypsometry.ocean_mode_m << ",\n";
    out << "        \"land_mode_m\": " << s.hypsometry.land_mode_m << ",\n";
    out << "        \"mode_separation_m\": " << s.hypsometry.mode_separation_m << ",\n";
    out << "        \"terrain_macro_correlation\": " << s.hypsometry.terrain_macro_correlation << "\n";
    out << "      },\n";
    out << "      \"coupling\": {\n";
    out << "        \"uplift_active_fraction\": " << s.coupling.uplift_active_fraction << ",\n";
    out << "        \"divergence_active_fraction\": " << s.coupling.divergence_active_fraction << ",\n";
    out << "        \"uplift_macro_excess_m\": " << s.coupling.uplift_macro_excess_m << ",\n";
    out << "        \"oceanic_divergence_macro_excess_m\": " << s.coupling.oceanic_divergence_macro_excess_m << ",\n";
    out << "        \"continental_divergence_macro_excess_m\": " << s.coupling.continental_divergence_macro_excess_m << "\n";
    out << "      }\n";
    out << "    }" << (trailing_comma ? "," : "") << "\n";
}

void write_json(const std::filesystem::path& path,
                const Options& options,
                const std::vector<SeedMetrics>& seeds,
                const std::vector<std::string>& suite_findings) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open "+path.string());
    out << std::setprecision(10);

    const auto land_range=minmax_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.land_fraction; });
    const auto plate_cv_range=minmax_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_cv; });
    const auto ratio_range=minmax_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_max_min_ratio; });
    const auto components_range=minmax_metric(seeds,[](const SeedMetrics& s){ return static_cast<double>(s.crust.high_affinity_component_count); });

    out << "{\n";
    out << "  \"schema\": \"worldsim.geology_plausibility.v1\",\n";
    out << "  \"method\": {\n";
    out << "    \"seed_start\": " << options.seed_start << ",\n";
    out << "    \"seed_count\": " << options.seed_count << ",\n";
    out << "    \"equal_area_probe_count\": " << options.samples << ",\n";
    out << "    \"cube_sphere_cover_level\": " << static_cast<unsigned>(options.cover_level) << ",\n";
    out << "    \"high_affinity_threshold\": 0.5,\n";
    out << "    \"uplift_active_threshold\": 0.05,\n";
    out << "    \"divergence_active_threshold\": 0.05\n";
    out << "  },\n";
    out << "  \"reference_context\": {\n";
    out << "    \"pb2002_plate_count\": 52,\n";
    out << "    \"pb2002_reported_power_law_area_range_sr_min\": 0.002,\n";
    out << "    \"pb2002_reported_power_law_area_range_sr_max\": 1.0,\n";
    out << "    \"earth_water_covered_surface_fraction_approx\": 0.71,\n";
    out << "    \"note\": \"Reference values guide warnings only; they are not scientific calibration gates.\"\n";
    out << "  },\n";
    out << "  \"aggregate\": {\n";
    out << "    \"plate_area_cv_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_cv; }) << ",\n";
    out << "    \"plate_area_cv_min\": " << plate_cv_range.first << ",\n";
    out << "    \"plate_area_cv_max\": " << plate_cv_range.second << ",\n";
    out << "    \"plate_area_max_min_ratio_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_max_min_ratio; }) << ",\n";
    out << "    \"plate_area_max_min_ratio_min\": " << ratio_range.first << ",\n";
    out << "    \"plate_area_max_min_ratio_max\": " << ratio_range.second << ",\n";
    out << "    \"plate_center_nearest_neighbor_cv_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.center_nearest_neighbor_cv; }) << ",\n";
    out << "    \"convergent_boundary_fraction_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.convergent_boundary_fraction; }) << ",\n";
    out << "    \"divergent_boundary_fraction_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.divergent_boundary_fraction; }) << ",\n";
    out << "    \"transform_boundary_fraction_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.transform_boundary_fraction; }) << ",\n";
    out << "    \"land_fraction_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.land_fraction; }) << ",\n";
    out << "    \"land_fraction_min\": " << land_range.first << ",\n";
    out << "    \"land_fraction_max\": " << land_range.second << ",\n";
    out << "    \"hypsometric_mode_separation_mean_m\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.mode_separation_m; }) << ",\n";
    out << "    \"high_affinity_component_count_mean\": " << mean_metric(seeds,[](const SeedMetrics& s){ return static_cast<double>(s.crust.high_affinity_component_count); }) << ",\n";
    out << "    \"high_affinity_component_count_min\": " << components_range.first << ",\n";
    out << "    \"high_affinity_component_count_max\": " << components_range.second << ",\n";
    out << "    \"uplift_macro_excess_mean_m\": " << mean_metric(seeds,[](const SeedMetrics& s){ return s.coupling.uplift_macro_excess_m; }) << "\n";
    out << "  },\n";
    out << "  \"findings\": [\n";
    for (std::size_t i=0;i<suite_findings.size();++i)
        out << "    " << std::quoted(suite_findings[i]) << (i+1<suite_findings.size() ? "," : "") << "\n";
    out << "  ],\n";
    out << "  \"seeds\": [\n";
    for (std::size_t i=0;i<seeds.size();++i)
        write_seed_json(out,seeds[i],i+1<seeds.size());
    out << "  ]\n";
    out << "}\n";
    if (!out) throw std::runtime_error("failed writing "+path.string());
}

void print_summary(const std::vector<SeedMetrics>& seeds,
                   const std::vector<std::string>& suite_findings) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout
        << "WORLDSIM_GEOLOGY_BENCHMARK_OK seeds=" << seeds.size()
        << " plate_area_cv_mean="
        << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_cv; })
        << " plate_ratio_mean="
        << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.area_max_min_ratio; })
        << " center_nn_cv_mean="
        << mean_metric(seeds,[](const SeedMetrics& s){ return s.plates.center_nearest_neighbor_cv; })
        << " land_fraction_mean="
        << mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.land_fraction; })
        << " mode_separation_m="
        << mean_metric(seeds,[](const SeedMetrics& s){ return s.hypsometry.mode_separation_m; })
        << '\n';
    for (const std::string& finding:suite_findings)
        std::cout << finding << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options=parse_options(argc,argv);
        std::filesystem::create_directories(options.output);

        std::vector<SeedMetrics> seeds;
        seeds.reserve(static_cast<std::size_t>(options.seed_count));
        for (std::uint64_t offset=0;offset<options.seed_count;++offset) {
            const std::uint64_t seed=options.seed_start+offset;
            seeds.push_back(evaluate_seed(seed,options));
        }

        const std::vector<std::string> suite_findings=findings(seeds);
        write_json(options.output/"geology_benchmark.json",options,seeds,suite_findings);
        print_summary(seeds,suite_findings);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_geology_benchmark: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
