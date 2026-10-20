#include "readers/reader.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <initializer_list>

namespace streamfind::raman::reader::wip_detail {

struct Tag {
    std::string name;
    std::uint32_t type = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::vector<Tag> children;
};

class Tree {
  public:
    explicit Tree(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    const std::vector<std::uint8_t> &bytes() const { return bytes_; }

    std::uint32_t u32(std::uint64_t at) const {
        require(at, 4);
        return static_cast<std::uint32_t>(bytes_[at]) |
               (static_cast<std::uint32_t>(bytes_[at + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes_[at + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes_[at + 3]) << 24);
    }

    std::uint64_t u64(std::uint64_t at) const {
        return static_cast<std::uint64_t>(u32(at)) |
               (static_cast<std::uint64_t>(u32(at + 4)) << 32);
    }

    Tag parse_tag(std::uint64_t &at, std::uint64_t limit, std::size_t depth,
                  std::size_t &total) const {
        if (depth > 48 || ++total > 4'000'000)
            throw std::runtime_error("WITec tag tree exceeds safety limits");
        const auto name_size = u32(at);
        if (name_size > 4096) throw std::runtime_error("WITec tag name is implausibly long");
        const auto header = at + 4 + static_cast<std::uint64_t>(name_size) + 20;
        require(at, 4 + static_cast<std::uint64_t>(name_size) + 20);
        if (header > limit) throw std::runtime_error("WITec tag header exceeds its parent");
        Tag tag;
        tag.name.assign(reinterpret_cast<const char *>(bytes_.data() + at + 4), name_size);
        tag.type = u32(at + 4 + name_size);
        tag.begin = u64(at + 8 + name_size);
        tag.end = u64(at + 16 + name_size);
        if (tag.begin < header || tag.end < tag.begin || tag.end > limit)
            throw std::runtime_error("WITec tag has invalid data bounds: " + tag.name);
        at = tag.end;
        if (tag.type == 0) {
            auto child_at = tag.begin;
            while (child_at < tag.end)
                tag.children.push_back(parse_tag(child_at, tag.end, depth + 1, total));
            if (child_at != tag.end) throw std::runtime_error("WITec child tags do not align");
        }
        return tag;
    }

    std::vector<Tag> parse_all(std::uint64_t at, std::uint64_t limit) const {
        std::vector<Tag> tags;
        std::size_t total = 0;
        while (at < limit) tags.push_back(parse_tag(at, limit, 0, total));
        if (at != limit) throw std::runtime_error("WITec top-level tags do not align");
        return tags;
    }

    std::vector<double> numbers(const Tag *tag) const {
        std::vector<double> out;
        if (tag == nullptr) return out;
        const std::uint64_t width = tag->type == 2 || tag->type == 4 ? 8 :
                                    tag->type == 3 || tag->type == 5 ? 4 : 0;
        if (width == 0 || (tag->end - tag->begin) % width != 0) return out;
        for (auto p = tag->begin; p < tag->end; p += width) {
            if (tag->type == 5)
                out.push_back(static_cast<double>(static_cast<std::int32_t>(u32(p))));
            else if (width == 8)
                out.push_back(std::bit_cast<double>(u64(p)));
            else
                out.push_back(static_cast<double>(std::bit_cast<float>(u32(p))));
        }
        return out;
    }

    std::vector<std::string> strings(const Tag *tag) const {
        std::vector<std::string> out;
        if (tag == nullptr || tag->type != 9) return out;
        auto p = tag->begin;
        while (p < tag->end) {
            if (tag->end - p < 4) return {};
            const auto n = u32(p);
            p += 4;
            if (n > tag->end - p) return {};
            out.emplace_back(reinterpret_cast<const char *>(bytes_.data() + p), n);
            p += n;
        }
        return out;
    }

    std::optional<std::int64_t> integer(const Tag *tag) const {
        const auto vals = numbers(tag);
        if (vals.empty() || !std::isfinite(vals.front())) return std::nullopt;
        return static_cast<std::int64_t>(vals.front());
    }

    bool boolean(const Tag *tag, bool fallback = false) const {
        if (tag == nullptr || tag->type != 8 || tag->end == tag->begin) return fallback;
        return bytes_[tag->begin] != 0;
    }

    const Tag *child(const Tag *tag, const std::string &name) const {
        if (tag == nullptr) return nullptr;
        const auto it = std::find_if(tag->children.begin(), tag->children.end(),
                                     [&](const Tag &candidate) { return candidate.name == name; });
        return it == tag->children.end() ? nullptr : &*it;
    }

    const Tag *path(const Tag *tag, std::initializer_list<const char *> names) const {
        for (const auto *name : names) tag = child(tag, name);
        return tag;
    }

  private:
    void require(std::uint64_t at, std::uint64_t size) const {
        if (at > bytes_.size() || size > bytes_.size() - at)
            throw std::runtime_error("WITec file is truncated or contains an out-of-range offset");
    }
    std::vector<std::uint8_t> bytes_;
};

struct Entry {
    std::string class_name;
    std::string caption;
    const Tag *tag = nullptr;
    const Tag *body = nullptr;
    std::int64_t id = -1;
};

double polynomial(const std::vector<double> &coefficients, double x) {
    double value = 0.0;
    for (auto it = coefficients.rbegin(); it != coefficients.rend(); ++it)
        value = value * x + *it;
    return value;
}

}  // namespace streamfind::raman::reader::wip_detail

namespace streamfind::raman::reader {

SpectrumFile read_wip_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback) {
    using namespace wip_detail;
    if (!spectrum_callback) throw std::invalid_argument("WIP spectrum callback is required");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open WITec project: " + path.string());
    const auto size = input.tellg();
    if (size < 8 || size > static_cast<std::streamoff>(2ULL * 1024 * 1024 * 1024))
        throw std::runtime_error("WITec project size is outside supported bounds");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
        throw std::runtime_error("cannot read WITec project: " + path.string());
    const std::string magic(reinterpret_cast<const char *>(bytes.data()), 8);
    if (magic != "WIT_PRCT" && magic != "WIT_PR06")
        throw std::runtime_error("unsupported WITec project signature: " + magic);

    Tree tree(std::move(bytes));
    const auto top = tree.parse_all(8, tree.bytes().size());
    const auto root_it = std::find_if(top.begin(), top.end(), [](const Tag &tag) { return tag.type == 0; });
    if (root_it == top.end()) throw std::runtime_error("WITec project contains no root tag");
    const Tag *root = &*root_it;
    const Tag *data = tree.child(root, "Data");
    if (data == nullptr) throw std::runtime_error("WITec project has no Data collection");

    std::vector<Entry> entries;
    std::map<std::int64_t, const Entry *> by_id;
    for (std::size_t i = 0; i < 100000; ++i) {
        const auto class_names = tree.strings(tree.child(data, "DataClassName " + std::to_string(i)));
        const auto *record = tree.child(data, "Data " + std::to_string(i));
        if (class_names.empty() || record == nullptr) break;
        Entry entry;
        entry.class_name = class_names.front();
        entry.tag = record;
        entry.body = tree.child(record, entry.class_name);
        const auto *td = tree.child(record, "TData");
        entry.id = tree.integer(tree.child(td, "ID")).value_or(-1);
        const auto captions = tree.strings(tree.child(td, "Caption"));
        if (!captions.empty()) entry.caption = captions.front();
        entries.push_back(std::move(entry));
    }
    for (const auto &entry : entries)
        if (entry.id >= 0) by_id.emplace(entry.id, &entry);

    SpectrumFile result;
    result.path = path;
    result.format = "witec_wip";
    result.axis_kind = "raman_shift";
    result.axis_unit = "cm^-1";
    result.signal_kind = "intensity";
    result.signal_unit = "CCD counts";
    result.metadata.push_back({"Container", "WITec Project", 0});
    result.metadata.push_back({"Signature", magic, 0});
    if (const auto version = tree.integer(tree.child(root, "Version")))
        result.metadata.push_back({"Format Version", std::to_string(*version), 0});

    for (const auto &entry : entries) {
        if (entry.class_name != "TDGraph" || entry.body == nullptr) continue;
        const auto *graph = entry.body;
        const auto sx = tree.integer(tree.child(graph, "SizeX")).value_or(1);
        const auto sy = tree.integer(tree.child(graph, "SizeY")).value_or(1);
        const auto point_count = tree.integer(tree.child(graph, "SizeGraph")).value_or(0);
        const auto *graph_data = tree.path(graph, {"GraphData", "Data"});
        const auto type = tree.integer(tree.path(graph, {"GraphData", "DataType"})).value_or(-1);
        if (sx < 1 || sy < 1 || point_count < 2 || point_count > 10'000'000 || graph_data == nullptr)
            continue;
        if (type != 9 && type != 10 && type != 1 && type != 2 && type != 3 && type != 4 && type != 5 && type != 6 && type != 7)
            continue;
        const std::uint64_t value_width = type == 1 || type == 10 ? 8 :
                                         type == 2 || type == 5 || type == 9 ? 4 :
                                         type == 3 || type == 6 ? 2 : 1;
        const auto spectrum_total = static_cast<std::uint64_t>(sx) * static_cast<std::uint64_t>(sy);
        const auto expected = spectrum_total * static_cast<std::uint64_t>(point_count) * value_width;
        if (expected != graph_data->end - graph_data->begin)
            throw std::runtime_error("WITec graph dimensions do not match payload size: " + entry.caption);

        std::vector<double> axis;
        const auto x_id = tree.integer(tree.child(graph, "XTransformationID"));
        const auto interp_id = tree.integer(tree.child(graph, "XInterpretationID"));
        const Entry *transform = x_id && by_id.contains(*x_id) ? by_id.at(*x_id) : nullptr;
        const Entry *interpretation = interp_id && by_id.contains(*interp_id) ? by_id.at(*interp_id) : nullptr;
        const auto *transform_body = transform ? transform->body : nullptr;
        const auto transform_type = tree.integer(tree.child(transform_body, "SpectralTransformationType")).value_or(-1);
        if (transform && transform->class_name == "TDSpectralTransformation") {
            axis.reserve(static_cast<std::size_t>(point_count));
            if (transform_type == 0) {
                auto coefficients = tree.numbers(tree.child(transform_body, "Polynom"));
                if (coefficients.size() > 3) coefficients.resize(3);
                for (std::int64_t p = 0; p < point_count && !coefficients.empty(); ++p)
                    axis.push_back(polynomial(coefficients, static_cast<double>(p)));
            } else if (transform_type == 2) {
                const auto coefficients = tree.numbers(tree.child(transform_body, "FreePolynom"));
                const auto order = tree.integer(tree.child(transform_body, "FreePolynomOrder")).value_or(-1);
                const auto first = tree.numbers(tree.child(transform_body, "FreePolynomStartBin"));
                const auto last = tree.numbers(tree.child(transform_body, "FreePolynomStopBin"));
                if (!coefficients.empty() && order >= 0 && !first.empty() && !last.empty()) {
                    const auto n = std::min(coefficients.size(), static_cast<std::size_t>(order + 1));
                    const std::vector<double> selected(coefficients.begin(), coefficients.begin() + n);
                    const auto lo = first.front();
                    const auto hi = std::max(lo, last.front());
                    for (std::int64_t p = 0; p < point_count; ++p) {
                        const auto x = static_cast<double>(p);
                        axis.push_back(x <= lo ? polynomial(selected, lo)
                                    : x >= hi ? polynomial(selected, hi)
                                              : polynomial(selected, x));
                    }
                }
            } else if (transform_type == 1) {
                const auto nc = tree.numbers(tree.child(transform_body, "nC"));
                const auto lc = tree.numbers(tree.child(transform_body, "LambdaC"));
                const auto gamma = tree.numbers(tree.child(transform_body, "Gamma"));
                const auto delta = tree.numbers(tree.child(transform_body, "Delta"));
                const auto order = tree.numbers(tree.child(transform_body, "m"));
                const auto grooves = tree.numbers(tree.child(transform_body, "d"));
                const auto pixel_width = tree.numbers(tree.child(transform_body, "x"));
                const auto focal = tree.numbers(tree.child(transform_body, "f"));
                if (!nc.empty() && !lc.empty() && !gamma.empty() && !delta.empty() && !order.empty() &&
                    !grooves.empty() && !pixel_width.empty() && !focal.empty()) {
                    const auto alpha = std::asin(lc.front() * order.front() / grooves.front() /
                        (2.0 * std::cos(gamma.front() / 2.0))) - gamma.front() / 2.0;
                    const auto beta_c = gamma.front() + alpha;
                    for (std::int64_t p = 0; p < point_count; ++p) {
                        const auto lh = focal.front() * std::cos(delta.front());
                        const auto hc = focal.front() * std::sin(delta.front());
                        const auto hn = pixel_width.front() * (nc.front() - static_cast<double>(p)) - hc;
                        const auto beta_n = beta_c - delta.front() - std::atan2(hn, lh);
                        axis.push_back(grooves.front() / order.front() * (std::sin(alpha) + std::sin(beta_n)));
                    }
                }
            }
        }
        if (axis.empty()) {
            result.axis_kind = "pixel";
            result.axis_unit = "point";
            axis.reserve(static_cast<std::size_t>(point_count));
            for (std::int64_t p = 0; p < point_count; ++p) axis.push_back(static_cast<double>(p));
        } else {
            const auto unit_index = tree.integer(tree.path(interpretation ? interpretation->tag : nullptr,
                                                          {"TDInterpretation", "UnitIndex"})).value_or(0);
            if (unit_index == 3) {
                const auto excitation = [&]() -> std::optional<double> {
                    const auto vals = tree.numbers(tree.child(interpretation ? interpretation->body : nullptr,
                                                               "ExcitationWaveLength"));
                    if (!vals.empty() && vals.front() > 0.0) return vals.front();
                    return std::nullopt;
                }();
                if (excitation) {
                    for (auto &v : axis) v = 1e7 * (1.0 / *excitation - 1.0 / v);
                    result.axis_kind = "raman_shift";
                    result.axis_unit = "cm^-1";
                    result.metadata.push_back({"Excitation Wavelength nm", std::to_string(*excitation), 0});
                } else {
                    result.axis_kind = "wavelength";
                    result.axis_unit = "nm";
                }
            } else if (unit_index == 2) {
                for (auto &v : axis) v = 1e7 / v;
                result.axis_kind = "wavenumber";
                result.axis_unit = "cm^-1";
            } else if (unit_index == 1) {
                for (auto &v : axis) v *= 1e-3;
                result.axis_kind = "wavelength";
                result.axis_unit = "um";
            } else if (unit_index == 4) {
                for (auto &v : axis) v = 1239.84193 / v;
                result.axis_kind = "energy";
                result.axis_unit = "eV";
            } else if (unit_index == 5) {
                for (auto &v : axis) v = 1239841.93 / v;
                result.axis_kind = "energy";
                result.axis_unit = "meV";
            } else if (unit_index == 6 || unit_index == 7) {
                const auto excitation = tree.numbers(tree.child(interpretation ? interpretation->body : nullptr,
                                                                  "ExcitationWaveLength"));
                if (!excitation.empty() && excitation.front() > 0.0) {
                    const auto factor = unit_index == 6 ? 1239.84193 : 1239841.93;
                    for (auto &v : axis) v = -factor * (1.0 / excitation.front() - 1.0 / v);
                    result.axis_kind = "energy_shift";
                    result.axis_unit = unit_index == 6 ? "eV" : "meV";
                } else {
                    result.axis_kind = "wavelength";
                    result.axis_unit = "nm";
                }
            } else if (unit_index == 0) {
                result.axis_kind = "wavelength";
                result.axis_unit = "nm";
            } else {
                result.axis_kind = "wavelength";
                result.axis_unit = "nm";
            }
        }

        const auto *z_interp = tree.path(graph, {"ZInterpretationID"});
        const auto z_id = tree.integer(z_interp);
        const Entry *signal_interp = z_id && by_id.contains(*z_id) ? by_id.at(*z_id) : nullptr;
        const auto signal_units = tree.strings(tree.child(signal_interp ? signal_interp->body : nullptr, "UnitName"));
        if (!signal_units.empty()) result.signal_unit = signal_units.front();
        result.metadata.push_back({"Graph Caption", entry.caption, result.spectrum_count});
        const auto has_marker = [&](const std::string &marker) {
            return entry.caption.find(marker) != std::string::npos;
        };
        const std::string processing_state = has_marker("(Sub BG)") ? "CRR + Sub BG"
            : has_marker("(CRR)") ? "CRR" : "Unannotated";
        result.metadata.push_back({"Graph Processing State", processing_state, result.spectrum_count});
        result.metadata.push_back({"WITec Data ID", std::to_string(entry.id), result.spectrum_count});

        const bool inverted = tree.boolean(tree.child(graph, "DataFieldInverted"));
        for (std::int64_t s = 0; s < sx * sy; ++s) {
            SpectrumFrame frame;
            frame.index = result.spectrum_count;
            frame.points.reserve(static_cast<std::size_t>(point_count));
            const auto x = s % sx;
            const auto y = s / sx;
            const auto stored_spectrum = inverted ? static_cast<std::uint64_t>(x + sx * y)
                                                  : static_cast<std::uint64_t>(y + sy * x);
            const auto start = graph_data->begin + stored_spectrum *
                               static_cast<std::uint64_t>(point_count) * value_width;
            for (std::int64_t p = 0; p < point_count; ++p) {
                const auto offset = start + static_cast<std::uint64_t>(p) * value_width;
                double intensity = 0.0;
                const auto &raw = tree.bytes();
                auto read_u = [&](std::size_t width) {
                    std::uint64_t value = 0;
                    for (std::size_t b = 0; b < width; ++b)
                        value |= static_cast<std::uint64_t>(raw[offset + b]) << (8 * b);
                    return value;
                };
                if (type == 9) intensity = std::bit_cast<float>(static_cast<std::uint32_t>(read_u(4)));
                else if (type == 10) intensity = std::bit_cast<double>(read_u(8));
                else if (type == 1) intensity = static_cast<double>(static_cast<std::int64_t>(read_u(8)));
                else if (type == 2) intensity = static_cast<double>(static_cast<std::int32_t>(read_u(4)));
                else if (type == 3) intensity = static_cast<double>(static_cast<std::int16_t>(read_u(2)));
                else if (type == 4) intensity = static_cast<double>(static_cast<std::int8_t>(read_u(1)));
                else if (type == 5) intensity = static_cast<double>(static_cast<std::uint32_t>(read_u(4)));
                else if (type == 6) intensity = static_cast<double>(static_cast<std::uint16_t>(read_u(2)));
                else intensity = static_cast<double>(read_u(1));
                if (!std::isfinite(axis[static_cast<std::size_t>(p)]) || !std::isfinite(intensity))
                    throw std::runtime_error("WITec graph contains non-finite spectral values");
                frame.points.push_back({axis[static_cast<std::size_t>(p)], intensity,
                                        static_cast<std::size_t>(p)});
            }
            result.point_count += frame.points.size();
            spectrum_callback(result, frame);
            ++result.spectrum_count;
        }
    }
    if (result.spectrum_count == 0) {
        std::size_t graph_count = 0;
        std::string first_graph_summary;
        for (const auto &entry : entries) {
            if (entry.class_name != "TDGraph") continue;
            ++graph_count;
            if (!first_graph_summary.empty()) continue;
            const auto *graph = entry.body;
            first_graph_summary = "first TDGraph '" + entry.caption + "': SizeX=" +
                std::to_string(tree.integer(tree.child(graph, "SizeX")).value_or(-1)) +
                ", SizeY=" + std::to_string(tree.integer(tree.child(graph, "SizeY")).value_or(-1)) +
                ", SizeGraph=" + std::to_string(tree.integer(tree.child(graph, "SizeGraph")).value_or(-1)) +
                ", data type=" + std::to_string(tree.integer(tree.path(graph, {"GraphData", "DataType"})).value_or(-1)) +
                (tree.path(graph, {"GraphData", "Data"}) == nullptr ? ", payload missing" : ", payload present");
        }
        throw std::runtime_error("WITec project contains no supported TDGraph spectra (" +
            std::to_string(graph_count) + " TDGraph entries)." +
            (first_graph_summary.empty() ? std::string{} : " " + first_graph_summary));
    }
    return result;
}

}  // namespace streamfind::raman::reader
