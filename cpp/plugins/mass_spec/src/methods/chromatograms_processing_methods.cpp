#include "methods/chromatograms_processing_methods.hpp"

#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
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

const std::vector<std::string> &chromatogram_columns() {
    static const std::vector<std::string> columns = {
        "analysis", "index", "chromatogram_id", "polarity",
        "precursor_mz", "activation_ce", "product_mz", "rt",
        "raw_intensity", "baseline", "intensity"};
    return columns;
}

Cells row_cells(const Json &row) {
    Cells cells;
    cells.reserve(chromatogram_columns().size());
    for (const auto &column : chromatogram_columns())
        cells.push_back(value_as_cell(row, column.c_str()));
    return cells;
}

std::optional<std::string> string_cell(const std::string &value) { return value; }
std::optional<std::string> integer_cell(int value) { return std::to_string(value); }
std::optional<std::string> number_cell(float value) {
    return std::isfinite(value)
        ? std::optional<std::string>(std::to_string(static_cast<double>(value)))
        : std::nullopt;
}

Cells chromatogram_cells(
    const std::string &analysis,
    const ::mass_spec::reader::MASS_SPEC_CHROMATOGRAMS_HEADERS &headers,
    std::size_t index, float rt, float intensity) {
    return {
        string_cell(analysis), integer_cell(headers.index[index]),
        string_cell(headers.chromatogram_id[index]), integer_cell(headers.polarity[index]),
        number_cell(headers.precursor_mz[index]), number_cell(headers.activation_ce[index]),
        number_cell(headers.product_mz[index]), std::to_string(rt),
        std::to_string(intensity), std::string("0"), std::to_string(intensity)};
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

std::vector<Cells> existing_rows(sdk::PluginProjectAccess &access) {
    std::vector<Cells> rows;
    for (const auto &row : access.query(
             "SELECT analysis,index,chromatogram_id,polarity,precursor_mz,activation_ce,product_mz,rt,raw_intensity,baseline,intensity FROM MASS_SPEC_CHROMATOGRAMS ORDER BY analysis"))
        rows.push_back(row_cells(row));
    return rows;
}

}  // namespace detail

Json load_chromatograms_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters) {
    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    auto retained = detail::existing_rows(access);
    const auto selected = detail::analyses(access, parameters);

    for (const auto &[analysis, path, index] : selected) {
        ::mass_spec::reader::MASS_SPEC_FILE file(path);
        file.select_analysis(index);
        const auto headers = file.get_chromatograms_headers();
        const auto arrays = file.get_chromatograms();
        std::vector<detail::Cells> loaded;
        for (std::size_t i = 0; i < arrays.size() && i < headers.chromatogram_id.size(); ++i) {
            const auto &id = headers.chromatogram_id[i];
            const bool keep = detail::matches(id, parameters) ^
                              parameters.value("invert", false);
            if (!keep || arrays[i].size() < 2) continue;

            retained.erase(std::remove_if(retained.begin(), retained.end(),
                [&](const detail::Cells &row) {
                    return row.size() >= 3 && row[0] && row[2] &&
                           *row[0] == analysis && *row[2] == id;
                }), retained.end());

            const auto &times = arrays[i][0];
            const auto &intensities = arrays[i][1];
            const auto count = std::min(times.size(), intensities.size());
            loaded.reserve(loaded.size() + count);
            for (std::size_t j = 0; j < count; ++j)
                loaded.push_back(detail::chromatogram_cells(analysis, headers, i,
                                                            times[j], intensities[j]));
        }
        retained.insert(retained.end(), loaded.begin(), loaded.end());
    }

    access.clear_table("MASS_SPEC_CHROMATOGRAMS");
    access.append("MASS_SPEC_CHROMATOGRAMS", detail::chromatogram_columns(), retained);
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
    auto rows = detail::existing_rows(access);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const detail::Cells &row) {
        if (row.size() < 8 || !row[0] || !row[7]) return false;
        if (!detail::selected_analysis(wanted, *row[0])) return false;
        const double rt = std::stod(*row[7]);
        return rt < minimum || rt > maximum;
    }), rows.end());

    access.clear_table("MASS_SPEC_CHROMATOGRAMS");
    access.append("MASS_SPEC_CHROMATOGRAMS", detail::chromatogram_columns(), rows);
    return detail::status("Chromatograms filtered by retention time.");
}

}  // namespace streamfind::mass_spec::processing
