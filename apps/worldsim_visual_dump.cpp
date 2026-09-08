#include "worldsim/tectonics.hpp"
#include "worldsim/terrain.hpp"
#include "worldsim/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using worldsim::TectonicModel;
using worldsim::TectonicSample;
using worldsim::TerrainGenerator;
using worldsim::TerrainSample;
using worldsim::Vec3d;

struct Rgb {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
};

struct Options {
    std::vector<std::uint64_t> seeds{42};
    int width=512;
    int height=256;
    std::filesystem::path output="out/visual-dump";
};

struct MapData {
    int width{};
    int height{};
    std::vector<float> elevation_m;
    std::vector<float> land_fraction;
    std::vector<float> boundary_forcing;
    std::vector<float> uplift_forcing;
    std::vector<float> divergence_forcing;
    std::vector<float> crust_affinity;
    std::vector<float> macro_elevation_m;
    std::vector<std::uint32_t> plate_id;
};

struct WeightedStats {
    double min=std::numeric_limits<double>::infinity();
    double max=-std::numeric_limits<double>::infinity();
    double weighted_sum{};
    double weight_sum{};

    void add(double value, double weight) {
        if (!std::isfinite(value)) throw std::runtime_error("non-finite map sample");
        min=std::min(min,value);
        max=std::max(max,value);
        weighted_sum+=value*weight;
        weight_sum+=weight;
    }

    [[nodiscard]] double mean() const {
        return weighted_sum/std::max(weight_sum,std::numeric_limits<double>::min());
    }
};

struct PairStats {
    double weight_sum{};
    double x_sum{};
    double y_sum{};
    double xx_sum{};
    double yy_sum{};
    double xy_sum{};
    double abs_delta_sum{};
    double max_abs_delta{};

    void add(double x, double y, double weight) {
        const double delta=std::abs(x-y);
        weight_sum+=weight;
        x_sum+=weight*x;
        y_sum+=weight*y;
        xx_sum+=weight*x*x;
        yy_sum+=weight*y*y;
        xy_sum+=weight*x*y;
        abs_delta_sum+=weight*delta;
        max_abs_delta=std::max(max_abs_delta,delta);
    }

    [[nodiscard]] double correlation() const {
        const double w=std::max(weight_sum,std::numeric_limits<double>::min());
        const double cov=xy_sum-x_sum*y_sum/w;
        const double vx=xx_sum-x_sum*x_sum/w;
        const double vy=yy_sum-y_sum*y_sum/w;
        if (vx<=0.0 || vy<=0.0) return 0.0;
        return cov/std::sqrt(vx*vy);
    }

    [[nodiscard]] double mean_abs_delta() const {
        return abs_delta_sum/std::max(weight_sum,std::numeric_limits<double>::min());
    }
};

[[nodiscard]] int parse_positive_int(std::string_view value, std::string_view name) {
    std::size_t consumed=0;
    const long parsed=std::stol(std::string(value),&consumed,10);
    if (consumed!=value.size() || parsed<2 || parsed>4096)
        throw std::invalid_argument(std::string(name)+" must be an integer in [2,4096]");
    return static_cast<int>(parsed);
}

[[nodiscard]] std::uint64_t parse_seed(std::string_view value) {
    std::size_t consumed=0;
    const auto parsed=std::stoull(std::string(value),&consumed,10);
    if (consumed!=value.size()) throw std::invalid_argument("seed must be an unsigned integer");
    return parsed;
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        auto require_value=[&](std::string_view name) -> std::string_view {
            if (i+1>=argc) throw std::invalid_argument(std::string(name)+" requires a value");
            ++i;
            return argv[i];
        };
        if (arg=="--seed") {
            options.seeds={parse_seed(require_value(arg))};
        } else if (arg=="--suite") {
            options.seeds={1,7,19,42,63};
        } else if (arg=="--width") {
            options.width=parse_positive_int(require_value(arg),arg);
        } else if (arg=="--height") {
            options.height=parse_positive_int(require_value(arg),arg);
        } else if (arg=="--output") {
            options.output=std::filesystem::path(require_value(arg));
        } else if (arg=="--help" || arg=="-h") {
            std::cout
                << "Usage: worldsim_visual_dump [--seed N | --suite] [--width N] [--height N] [--output DIR]\n"
                << "Writes core world-generation PFM/PPM layers plus metrics.json.\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: "+std::string(arg));
        }
    }
    const std::int64_t samples=static_cast<std::int64_t>(options.width)*options.height;
    if (samples>2'097'152) throw std::invalid_argument("map may contain at most 2097152 samples");
    return options;
}

[[nodiscard]] Vec3d pixel_direction(int x, int y, int width, int height) {
    const double v=(static_cast<double>(y)+0.5)/static_cast<double>(height);
    const double latitude=(0.5-v)*worldsim::kPi;
    const double sin_lat=std::sin(latitude);
    const double cos_lat=std::cos(latitude);
    const double u=(static_cast<double>(x)+0.5)/static_cast<double>(width);
    const double longitude=(2.0*u-1.0)*worldsim::kPi;
    return {
        cos_lat*std::cos(longitude),
        cos_lat*std::sin(longitude),
        sin_lat
    };
}

[[nodiscard]] double row_weight(int y, int height) {
    const double v=(static_cast<double>(y)+0.5)/static_cast<double>(height);
    const double latitude=(0.5-v)*worldsim::kPi;
    return std::max(0.0,std::cos(latitude));
}

[[nodiscard]] MapData sample_map(std::uint64_t seed, int width, int height) {
    MapData data;
    data.width=width;
    data.height=height;
    const std::size_t count=static_cast<std::size_t>(width)*static_cast<std::size_t>(height);
    data.elevation_m.resize(count);
    data.land_fraction.resize(count);
    data.boundary_forcing.resize(count);
    data.uplift_forcing.resize(count);
    data.divergence_forcing.resize(count);
    data.crust_affinity.resize(count);
    data.macro_elevation_m.resize(count);
    data.plate_id.resize(count);

    const TerrainGenerator terrain(seed);
    const TectonicModel tectonics(seed);
    for (int y=0;y<height;++y) {
        for (int x=0;x<width;++x) {
            const std::size_t index=static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x);
            const Vec3d direction=pixel_direction(x,y,width,height);
            const TerrainSample terrain_sample=terrain.sample_direction(direction);
            const TectonicSample tectonic_sample=tectonics.sample_direction(direction);
            data.elevation_m[index]=static_cast<float>(terrain_sample.elevation_m);
            data.land_fraction[index]=static_cast<float>(terrain_sample.land_fraction);
            data.boundary_forcing[index]=static_cast<float>(tectonic_sample.boundary_forcing);
            data.uplift_forcing[index]=static_cast<float>(tectonic_sample.uplift_forcing);
            data.divergence_forcing[index]=static_cast<float>(tectonic_sample.divergence_forcing);
            data.crust_affinity[index]=static_cast<float>(tectonic_sample.continental_affinity);
            data.macro_elevation_m[index]=static_cast<float>(tectonic_sample.macro_elevation_m);
            data.plate_id[index]=tectonic_sample.plate_id;
        }
    }
    return data;
}

void write_le_float(std::ofstream& out, float value) {
    const std::uint32_t bits=std::bit_cast<std::uint32_t>(value);
    const std::array<char,4> bytes{
        static_cast<char>(bits&0xffU),
        static_cast<char>((bits>>8U)&0xffU),
        static_cast<char>((bits>>16U)&0xffU),
        static_cast<char>((bits>>24U)&0xffU)
    };
    out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
}

void write_pfm(const std::filesystem::path& path, const std::vector<float>& values, int width, int height) {
    if (values.size()!=static_cast<std::size_t>(width)*static_cast<std::size_t>(height))
        throw std::invalid_argument("PFM value count mismatch");
    std::ofstream out(path,std::ios::binary);
    if (!out) throw std::runtime_error("cannot open "+path.string());
    out << "Pf\n" << width << ' ' << height << "\n-1.0\n";
    for (int y=height-1;y>=0;--y) {
        const std::size_t row=static_cast<std::size_t>(y)*static_cast<std::size_t>(width);
        for (int x=0;x<width;++x) write_le_float(out,values[row+static_cast<std::size_t>(x)]);
    }
    if (!out) throw std::runtime_error("failed writing "+path.string());
}

[[nodiscard]] Rgb lerp(Rgb a, Rgb b, double t) {
    t=std::clamp(t,0.0,1.0);
    const auto channel=[&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<double>(x)+(static_cast<double>(y)-x)*t));
    };
    return {channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b)};
}

[[nodiscard]] Rgb elevation_color(double elevation_m) {
    if (elevation_m<0.0) {
        const double t=std::clamp((elevation_m+6000.0)/6000.0,0.0,1.0);
        if (t<0.60) return lerp({8,20,58},{24,79,145},t/0.60);
        return lerp({24,79,145},{118,177,202},(t-0.60)/0.40);
    }
    if (elevation_m<1000.0) return lerp({74,126,66},{156,145,91},elevation_m/1000.0);
    if (elevation_m<3500.0) return lerp({156,145,91},{137,98,72},(elevation_m-1000.0)/2500.0);
    return lerp({137,98,72},{240,240,236},(elevation_m-3500.0)/4500.0);
}

[[nodiscard]] Rgb signed_color(double value) {
    const Rgb neutral{68,72,80};
    const double magnitude=std::pow(std::clamp(std::abs(value)/0.55,0.0,1.0),0.72);
    return value>=0.0 ? lerp(neutral,{238,77,46},magnitude) : lerp(neutral,{46,108,235},magnitude);
}

[[nodiscard]] Rgb positive_color(double value, Rgb active) {
    const double t=std::pow(std::clamp(value,0.0,1.0),0.55);
    return lerp({28,31,36},active,t);
}

[[nodiscard]] Rgb crust_color(double affinity) {
    return lerp({12,39,70},{197,171,112},std::clamp(affinity,0.0,1.0));
}

constexpr std::array<Rgb,TectonicModel::kPlateCount> kPlateColors{{
    {214,89,82},{83,139,214},{99,178,112},{221,164,73},
    {151,105,199},{64,174,177},{203,107,161},{145,145,78},
    {105,126,184},{194,127,74},{84,164,139},{184,100,103},
    {116,154,73},{180,137,188},{80,146,163},{171,150,92}
}};

void write_ppm(const std::filesystem::path& path, const std::vector<Rgb>& pixels, int width, int height) {
    if (pixels.size()!=static_cast<std::size_t>(width)*static_cast<std::size_t>(height))
        throw std::invalid_argument("PPM pixel count mismatch");
    std::ofstream out(path,std::ios::binary);
    if (!out) throw std::runtime_error("cannot open "+path.string());
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (const Rgb color:pixels) {
        const std::array<char,3> bytes{
            static_cast<char>(color.r),static_cast<char>(color.g),static_cast<char>(color.b)
        };
        out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) throw std::runtime_error("failed writing "+path.string());
}

template<class ColorFn>
void write_field_preview(const std::filesystem::path& path,
                         const std::vector<float>& values,
                         int width,
                         int height,
                         ColorFn color_fn) {
    std::vector<Rgb> pixels;
    pixels.reserve(values.size());
    for (float value:values) pixels.push_back(color_fn(static_cast<double>(value)));
    write_ppm(path,pixels,width,height);
}

void write_plate_preview(const std::filesystem::path& path, const MapData& data) {
    std::vector<Rgb> pixels(data.plate_id.size());
    for (int y=0;y<data.height;++y) {
        for (int x=0;x<data.width;++x) {
            const std::size_t i=static_cast<std::size_t>(y)*static_cast<std::size_t>(data.width)+static_cast<std::size_t>(x);
            const std::uint32_t plate=data.plate_id[i];
            bool boundary=false;
            const int left_x=(x+data.width-1)%data.width;
            const std::size_t left=static_cast<std::size_t>(y)*static_cast<std::size_t>(data.width)+static_cast<std::size_t>(left_x);
            boundary=boundary || data.plate_id[left]!=plate;
            if (y>0) {
                const std::size_t up=static_cast<std::size_t>(y-1)*static_cast<std::size_t>(data.width)+static_cast<std::size_t>(x);
                boundary=boundary || data.plate_id[up]!=plate;
            }
            pixels[i]=boundary ? Rgb{18,20,24} : kPlateColors[plate%kPlateColors.size()];
        }
    }
    write_ppm(path,pixels,data.width,data.height);
}

[[nodiscard]] double weighted_fraction(const std::vector<float>& values,
                                       int width,
                                       int height,
                                       auto predicate) {
    double numerator=0.0;
    double denominator=0.0;
    for (int y=0;y<height;++y) {
        const double weight=row_weight(y,height);
        for (int x=0;x<width;++x) {
            const std::size_t i=static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x);
            denominator+=weight;
            if (predicate(static_cast<double>(values[i]))) numerator+=weight;
        }
    }
    return numerator/std::max(denominator,std::numeric_limits<double>::min());
}

[[nodiscard]] WeightedStats field_stats(const std::vector<float>& values, int width, int height) {
    WeightedStats stats;
    for (int y=0;y<height;++y) {
        const double weight=row_weight(y,height);
        for (int x=0;x<width;++x) {
            const std::size_t i=static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x);
            stats.add(static_cast<double>(values[i]),weight);
        }
    }
    return stats;
}

[[nodiscard]] double seam_ratio(const std::vector<float>& values, int width, int height) {
    double seam_sum=0.0;
    double seam_weight=0.0;
    double internal_sum=0.0;
    double internal_weight=0.0;
    for (int y=0;y<height;++y) {
        const double weight=row_weight(y,height);
        const std::size_t row=static_cast<std::size_t>(y)*static_cast<std::size_t>(width);
        seam_sum+=weight*std::abs(static_cast<double>(values[row])-static_cast<double>(values[row+static_cast<std::size_t>(width-1)]));
        seam_weight+=weight;
        for (int x=1;x<width;++x) {
            internal_sum+=weight*std::abs(
                static_cast<double>(values[row+static_cast<std::size_t>(x)])-
                static_cast<double>(values[row+static_cast<std::size_t>(x-1)])
            );
            internal_weight+=weight;
        }
    }
    const double seam_mean=seam_sum/std::max(seam_weight,std::numeric_limits<double>::min());
    const double internal_mean=internal_sum/std::max(internal_weight,std::numeric_limits<double>::min());
    return seam_mean/std::max(internal_mean,1.0e-12);
}

[[nodiscard]] PairStats compare_fields(const std::vector<float>& a,
                                       const std::vector<float>& b,
                                       int width,
                                       int height) {
    PairStats stats;
    for (int y=0;y<height;++y) {
        const double weight=row_weight(y,height);
        for (int x=0;x<width;++x) {
            const std::size_t i=static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x);
            stats.add(static_cast<double>(a[i]),static_cast<double>(b[i]),weight);
        }
    }
    return stats;
}

void write_metrics(const std::filesystem::path& path, std::uint64_t seed, const MapData& data) {
    const WeightedStats elevation=field_stats(data.elevation_m,data.width,data.height);
    const WeightedStats macro=field_stats(data.macro_elevation_m,data.width,data.height);
    const WeightedStats crust=field_stats(data.crust_affinity,data.width,data.height);
    const PairStats elevation_vs_macro=compare_fields(data.elevation_m,data.macro_elevation_m,data.width,data.height);

    double land_sum=0.0;
    double weight_sum=0.0;
    std::array<bool,TectonicModel::kPlateCount> plates{};
    for (int y=0;y<data.height;++y) {
        const double weight=row_weight(y,data.height);
        for (int x=0;x<data.width;++x) {
            const std::size_t i=static_cast<std::size_t>(y)*static_cast<std::size_t>(data.width)+static_cast<std::size_t>(x);
            land_sum+=weight*static_cast<double>(data.land_fraction[i]);
            weight_sum+=weight;
            if (data.plate_id[i]<plates.size()) plates[data.plate_id[i]]=true;
        }
    }
    const auto plate_count=static_cast<unsigned>(std::count(plates.begin(),plates.end(),true));

    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open "+path.string());
    out << std::setprecision(10);
    out << "{\n";
    out << "  \"seed\": " << seed << ",\n";
    out << "  \"width\": " << data.width << ",\n";
    out << "  \"height\": " << data.height << ",\n";
    out << "  \"plate_count_seen\": " << plate_count << ",\n";
    out << "  \"elevation\": {\n";
    out << "    \"min_m\": " << elevation.min << ",\n";
    out << "    \"max_m\": " << elevation.max << ",\n";
    out << "    \"area_weighted_mean_m\": " << elevation.mean() << ",\n";
    out << "    \"area_weighted_land_fraction\": " << land_sum/std::max(weight_sum,std::numeric_limits<double>::min()) << ",\n";
    out << "    \"longitude_seam_ratio\": " << seam_ratio(data.elevation_m,data.width,data.height) << "\n";
    out << "  },\n";
    out << "  \"macro_elevation\": {\n";
    out << "    \"min_m\": " << macro.min << ",\n";
    out << "    \"max_m\": " << macro.max << ",\n";
    out << "    \"area_weighted_mean_m\": " << macro.mean() << ",\n";
    out << "    \"longitude_seam_ratio\": " << seam_ratio(data.macro_elevation_m,data.width,data.height) << "\n";
    out << "  },\n";
    out << "  \"terrain_vs_macro\": {\n";
    out << "    \"area_weighted_correlation\": " << elevation_vs_macro.correlation() << ",\n";
    out << "    \"area_weighted_mean_abs_residual_m\": " << elevation_vs_macro.mean_abs_delta() << ",\n";
    out << "    \"max_abs_residual_m\": " << elevation_vs_macro.max_abs_delta << "\n";
    out << "  },\n";
    out << "  \"crust\": {\n";
    out << "    \"area_weighted_mean_affinity\": " << crust.mean() << ",\n";
    out << "    \"transition_fraction_0_05_0_95\": "
        << weighted_fraction(data.crust_affinity,data.width,data.height,[](double v){ return v>0.05 && v<0.95; }) << ",\n";
    out << "    \"longitude_seam_ratio\": " << seam_ratio(data.crust_affinity,data.width,data.height) << "\n";
    out << "  },\n";
    out << "  \"tectonic_response\": {\n";
    out << "    \"uplift_active_fraction_0_05\": "
        << weighted_fraction(data.uplift_forcing,data.width,data.height,[](double v){ return v>=0.05; }) << ",\n";
    out << "    \"divergence_active_fraction_0_05\": "
        << weighted_fraction(data.divergence_forcing,data.width,data.height,[](double v){ return v>=0.05; }) << ",\n";
    out << "    \"uplift_longitude_seam_ratio\": " << seam_ratio(data.uplift_forcing,data.width,data.height) << ",\n";
    out << "    \"divergence_longitude_seam_ratio\": " << seam_ratio(data.divergence_forcing,data.width,data.height) << "\n";
    out << "  }\n";
    out << "}\n";
    if (!out) throw std::runtime_error("failed writing "+path.string());
}

void write_seed(std::uint64_t seed, const Options& options) {
    const std::filesystem::path dir=options.output/("seed-"+std::to_string(seed));
    std::filesystem::create_directories(dir);
    const MapData data=sample_map(seed,options.width,options.height);

    write_pfm(dir/"elevation.pfm",data.elevation_m,data.width,data.height);
    write_pfm(dir/"macro_elevation.pfm",data.macro_elevation_m,data.width,data.height);
    write_pfm(dir/"crust_affinity.pfm",data.crust_affinity,data.width,data.height);
    write_pfm(dir/"boundary_forcing.pfm",data.boundary_forcing,data.width,data.height);
    write_pfm(dir/"uplift_forcing.pfm",data.uplift_forcing,data.width,data.height);
    write_pfm(dir/"divergence_forcing.pfm",data.divergence_forcing,data.width,data.height);

    write_field_preview(dir/"elevation.ppm",data.elevation_m,data.width,data.height,elevation_color);
    write_field_preview(dir/"macro_elevation.ppm",data.macro_elevation_m,data.width,data.height,elevation_color);
    write_field_preview(dir/"crust_affinity.ppm",data.crust_affinity,data.width,data.height,crust_color);
    write_field_preview(dir/"boundary_forcing.ppm",data.boundary_forcing,data.width,data.height,signed_color);
    write_field_preview(dir/"uplift_forcing.ppm",data.uplift_forcing,data.width,data.height,[](double v){ return positive_color(v,{239,92,49}); });
    write_field_preview(dir/"divergence_forcing.ppm",data.divergence_forcing,data.width,data.height,[](double v){ return positive_color(v,{55,124,238}); });

    std::vector<float> signed_response(data.uplift_forcing.size());
    for (std::size_t i=0;i<signed_response.size();++i)
        signed_response[i]=data.uplift_forcing[i]-data.divergence_forcing[i];
    write_pfm(dir/"tectonic_response.pfm",signed_response,data.width,data.height);
    write_field_preview(dir/"tectonic_response.ppm",signed_response,data.width,data.height,signed_color);
    write_plate_preview(dir/"plates.ppm",data);
    write_metrics(dir/"metrics.json",seed,data);

    std::cout << "WORLDSIM_VISUAL_DUMP_OK seed=" << seed
              << " size=" << data.width << 'x' << data.height
              << " output=" << dir.string() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options=parse_options(argc,argv);
        for (const std::uint64_t seed:options.seeds) write_seed(seed,options);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_visual_dump: " << e.what() << '\n';
        return 1;
    }
}
