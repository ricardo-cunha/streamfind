#include "methods/chromatograms_processing_methods.hpp"

#include "operations/operations.hpp"
#include "readers/reader.hpp"
#include "utils/chromatogram_peaks.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

namespace streamfind::mass_spec::processing {
namespace detail {
using Cells = std::vector<std::optional<std::string>>;
using Analysis = std::tuple<std::string, std::string, int>;

std::optional<std::string> value_as_cell(const Json &row, const char *name) {
    const auto it = row.find(name);
    if (it == row.end() || it->is_null()) return std::nullopt;
    return it->is_string() ? it->get<std::string>() : it->dump();
}

std::optional<std::string> string_cell(const std::string &value) { return value; }
std::optional<std::string> integer_cell(int value) { return std::to_string(value); }
std::optional<std::string> number_cell(float value) {
    return std::isfinite(value)
        ? std::optional<std::string>(std::to_string(static_cast<double>(value)))
        : std::nullopt;
}

// --- Point columns: signal data only, no duplicated channel metadata ---

const std::vector<std::string> &point_columns() {
    static const std::vector<std::string> columns = {
        "analysis", "index", "rt", "raw_intensity", "baseline", "intensity"};
    return columns;
}

Cells point_cells(const Json &row) {
    Cells cells;
    cells.reserve(point_columns().size());
    for (const auto &column : point_columns())
        cells.push_back(value_as_cell(row, column.c_str()));
    return cells;
}

Cells make_point_cells(
    const std::string &analysis, int index, float rt, float intensity) {
    return {
        string_cell(analysis), integer_cell(index), std::to_string(rt),
        std::to_string(intensity), std::string("0"), std::to_string(intensity)};
}

// --- Header columns: all channel metadata persisted per chromatogram ---

const std::vector<std::string> &header_columns() {
    static const std::vector<std::string> columns = {
        "analysis", "index", "chromatogram_id", "array_length",
        "polarity", "precursor_mz", "activation_ce", "product_mz",
        "signal_type", "chromatogram_type", "detector", "channel",
        "units", "wavelength_nm", "interval_ms",
        "start_time", "end_time", "intensity_multiplier"};
    return columns;
}

Cells header_cells_from_reader(
    const std::string &analysis,
    const ::mass_spec::reader::MASS_SPEC_CHROMATOGRAMS_HEADERS &headers,
    std::size_t i) {
    return {
        string_cell(analysis),
        integer_cell(headers.index[i]),
        string_cell(headers.chromatogram_id[i]),
        integer_cell(headers.array_length[i]),
        integer_cell(headers.polarity[i]),
        number_cell(headers.precursor_mz[i]),
        number_cell(headers.activation_ce[i]),
        number_cell(headers.product_mz[i]),
        string_cell(headers.signal_type[i]),
        string_cell(headers.chromatogram_type[i]),
        string_cell(headers.detector[i]),
        string_cell(headers.channel[i]),
        string_cell(headers.units[i]),
        number_cell(headers.wavelength_nm[i]),
        number_cell(headers.interval_ms[i]),
        number_cell(headers.start_time[i]),
        number_cell(headers.end_time[i]),
        number_cell(headers.intensity_multiplier[i])};
}

Cells existing_point_rows_from_query(const Json &row) {
    Cells cells;
    cells.reserve(point_columns().size());
    for (const auto &column : point_columns())
        cells.push_back(value_as_cell(row, column.c_str()));
    return cells;
}

std::vector<Cells> existing_points(sdk::PluginProjectAccess &access) {
    std::vector<Cells> rows;
    for (const auto &row : access.query(
             "SELECT analysis,index,rt,raw_intensity,baseline,intensity "
             "FROM MASS_SPEC_CHROMATOGRAMS ORDER BY analysis"))
        rows.push_back(existing_point_rows_from_query(row));
    return rows;
}

std::vector<Analysis> analyses(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto wanted = parameters.value("analysis_names", Json::array());
    std::vector<Analysis> result;
    for (const auto &row : access.query(
             "SELECT analysis,file_path,analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis")) {
        const auto name = row.at("analysis").get<std::string>();
        bool selected = wanted.empty();
        for (const auto &value : wanted)
            selected = selected || value.get<std::string>() == name;
        if (!selected) continue;

        int index = 0;
        if (const auto it = row.find("analysis_index"); it != row.end() && !it->is_null()) {
            const auto text = it->get<std::string>();
            index = text.empty() ? 0 : std::stoi(text);
        }
        result.emplace_back(name, row.at("file_path").get<std::string>(), index);
    }
    if (result.empty())
        throw Error(ErrorCode::InvalidArgument,
                    "No analyses available for chromatogram processing.");
    return result;
}

bool matches(const std::string &value, const Json &parameters) {
    const auto patterns = parameters.value("chromatogram_id_regex", Json::array());
    if (patterns.empty()) return true;  // No filter = match everything.
    const auto flags = std::regex::ECMAScript |
        (parameters.value("ignore_case", true)
             ? std::regex::icase
             : std::regex_constants::syntax_option_type{});
    for (const auto &pattern : patterns) {
        try {
            if (std::regex_search(value, std::regex(pattern.get<std::string>(), flags)))
                return true;
        } catch (const std::regex_error &) {
        }
    }
    return false;
}

bool selected_analysis(const Json &wanted, const std::string &analysis) {
    if (wanted.empty()) return true;
    for (const auto &value : wanted)
        if (value.get<std::string>() == analysis) return true;
    return false;
}

Json status(const char *message) {
    return Json{{"status", "finished"}, {"info", message}};
}

}  // namespace detail

Json load_chromatograms_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters) {
    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    access.require_table("MASS_SPEC_CHROMATOGRAMS_HEADERS");
    auto retained_points = detail::existing_points(access);
    const auto selected = detail::analyses(access, parameters);

    // Track analysis+index pairs being loaded so we can delete old data.
    std::vector<std::pair<std::string, int>> loaded_keys;

    // Collect new point rows to insert.
    std::vector<detail::Cells> new_points;

    // Group analyses by file path so we open each file only once.
    std::map<std::string, std::vector<std::pair<std::string, int>>> by_path;
    for (const auto &[analysis, path, index] : selected)
        by_path[path].emplace_back(analysis, index);

    for (auto &[path, analyses] : by_path) {
        ::mass_spec::reader::MASS_SPEC_FILE file(path);
        for (auto &[analysis, index] : analyses) {
        try {
        file.select_analysis(index);
        const auto arrays = file.get_chromatograms();
        // Headers were already populated during add_analyses.
        // Read them from DuckDB to avoid re-parsing.
        std::vector<std::pair<std::string, int>> hdr_map;
        for (const auto &row : access.query(
                 "SELECT chromatogram_id, index FROM MASS_SPEC_CHROMATOGRAMS_HEADERS "
                 "WHERE analysis = '" + analysis + "' ORDER BY index"))
            hdr_map.push_back({row.at("chromatogram_id").get<std::string>(),
                               std::stoi(row.at("index").get<std::string>())});
        for (std::size_t i = 0; i < arrays.size() && i < hdr_map.size(); ++i) {
            const auto &id = hdr_map[i].first;
            const bool keep = detail::matches(id, parameters) ^
                              parameters.value("invert", false);
            if (!keep || arrays[i].size() < 2) continue;

            const int idx = hdr_map[i].second;
            loaded_keys.emplace_back(analysis, idx);

            const auto &times = arrays[i][0];
            const auto &intensities = arrays[i][1];
            const auto count = std::min(times.size(), intensities.size());
            for (std::size_t j = 0; j < count; ++j)
                new_points.push_back(detail::make_point_cells(
                    analysis, idx, times[j], intensities[j]));
        }
        } catch (const std::exception &error) {
            throw Error(ErrorCode::InvalidArgument,
                        "Failed to load SCIEX chromatograms for analysis " +
                        analysis + ": " + error.what());
        }
        }
    }

    // Clear chromatogram points and re-append.
    // Headers are already populated during add_analyses.
    access.clear_table("MASS_SPEC_CHROMATOGRAMS");

    if (!new_points.empty())
        access.append("MASS_SPEC_CHROMATOGRAMS", detail::point_columns(), new_points);

    return detail::status("Chromatograms loaded.");
}

Json filter_chromatograms_retention_time_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters) {
    const double minimum = parameters.at("rt_min").get<double>();
    const double maximum = parameters.at("rt_max").get<double>();
    if (minimum >= maximum)
        throw Error(ErrorCode::InvalidArgument, "rt_min must be less than rt_max.");

    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    const auto wanted = parameters.value("analysis_names", Json::array());
    auto rows = detail::existing_points(access);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const detail::Cells &row) {
        if (row.size() < 6 || !row[0] || !row[2]) return false;
        if (!detail::selected_analysis(wanted, *row[0])) return false;
        const double rt = std::stod(*row[2]);
        return rt < minimum || rt > maximum;
    }), rows.end());

    access.clear_table("MASS_SPEC_CHROMATOGRAMS");
    access.append("MASS_SPEC_CHROMATOGRAMS", detail::point_columns(), rows);
    return detail::status("Chromatograms filtered by retention time.");
}

Json find_chromatogram_peaks_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters) {
    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    access.require_table("MASS_SPEC_CHROMATOGRAMS_HEADERS");

    const auto wanted_analyses = parameters.value("analysis_names", Json::array());
    const auto wanted_indices = parameters.value("indices", Json::array());
    const bool merge = parameters.value("merge", true);
    const double merge_distance = parameters.value("merge_distance", 12.0);  // seconds
    const double min_peak_height = parameters.value("min_peak_height", 0.0);
    const double min_peak_distance = parameters.value("min_peak_distance", 9.0);  // seconds
    const double min_peak_width = parameters.value("min_peak_width", 0.12);  // seconds
    const double max_peak_width = parameters.value("max_peak_width", 120.0);  // seconds
    const double min_snr = parameters.value("min_snr", 10.0);

    // Load all chromatogram points grouped by analysis+index.
    std::string query =
        "SELECT c.analysis, c.index, c.rt, c.raw_intensity "
        "FROM MASS_SPEC_CHROMATOGRAMS c WHERE 1=1";
    if (!wanted_analyses.empty()) {
        query += " AND c.analysis IN (";
        for (std::size_t i = 0; i < wanted_analyses.size(); ++i)
            query += (i ? "," : "") + ::streamfind::mass_spec::detail::sql(wanted_analyses[i].get<std::string>());
        query += ")";
    }
    if (!wanted_indices.empty()) {
        query += " AND c.index IN (";
        for (std::size_t i = 0; i < wanted_indices.size(); ++i)
            query += (i ? "," : "") + std::to_string(wanted_indices[i].get<int>());
        query += ")";
    }
    query += " ORDER BY c.analysis, c.index, c.rt";

    // Group points by analysis+index.
    using PeakData = ::streamfind::mass_spec::detail::ChromatogramData;
    std::map<std::pair<std::string, int>, PeakData> groups;
    for (const auto &row : access.query(query)) {
        const auto analysis = row.at("analysis").get<std::string>();
        const int idx = std::stoi(row.at("index").get<std::string>());
        const double rt = std::stod(row.at("rt").get<std::string>());
        const double intensity = std::stod(row.at("raw_intensity").get<std::string>());
        auto &chrom = groups[std::make_pair(analysis, idx)];
        chrom.chromatogram_index = idx;
        chrom.rt.push_back(rt);
        chrom.intensity.push_back(intensity);
    }

    if (groups.empty())
        return detail::status("No chromatograms loaded for peak detection.");

    // Run peak detection on each chromatogram.
    std::vector<detail::Cells> peak_rows;
    int total_peaks = 0;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        const auto &key = it->first;
        auto &chrom = it->second;
        auto peaks = ::streamfind::mass_spec::detail::find_peaks(
            chrom, merge, merge_distance, min_peak_height,
            min_peak_distance, min_peak_width, max_peak_width, min_snr);
        for (auto &pk : peaks) {
            ++total_peaks;
            pk.peak_id = total_peaks;
            peak_rows.push_back({
                detail::string_cell(key.first),
                detail::integer_cell(pk.chromatogram_index),
                detail::integer_cell(pk.peak_id),
                detail::number_cell(pk.rt),
                detail::number_cell(pk.rt_start),
                detail::number_cell(pk.rt_end),
                detail::number_cell(pk.height),
                detail::number_cell(pk.raw_height),
                detail::number_cell(pk.area),
                detail::number_cell(pk.raw_area),
                detail::number_cell(pk.baseline_area),
                detail::number_cell(pk.width),
                detail::number_cell(pk.fwhm),
                detail::number_cell(pk.snr),
                detail::number_cell(pk.asymmetry),
                detail::number_cell(pk.sharpness),
                detail::number_cell(pk.plates),
                detail::integer_cell(pk.number_points),
                detail::string_cell("streamfind.integration.v1"),
                detail::string_cell("integrated"),
                detail::string_cell("false")});
        }
    }

    // Persist peaks.
    access.clear_table("MASS_SPEC_CHROMATOGRAM_PEAKS");
    if (!peak_rows.empty()) {
        static const std::vector<std::string> peak_columns = {
            "analysis", "chromatogram_index", "peak_id",
            "rt", "rt_start", "rt_end",
            "height", "raw_height", "area", "raw_area", "baseline_area",
            "width", "fwhm", "snr", "asymmetry", "sharpness", "plates",
            "number_points", "integration_algorithm", "integration_status",
            "manual_override"};
        access.append("MASS_SPEC_CHROMATOGRAM_PEAKS", peak_columns, peak_rows);
    }

    auto msg = "Found " + std::to_string(total_peaks) + " chromatographic peak(s).";
    return detail::status(msg.c_str());
}

}  // namespace streamfind::mass_spec::processing
