#include "streamfind/mass_spec/chromatograms_processing_methods.hpp"

#include "streamfind/mass_spec/reader.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

namespace streamfind::mass_spec::processing {
namespace detail {

std::string sql(const std::string &value) {
    std::string result = "'";
    for (const char c : value) result += c == '\'' ? "''" : std::string(1, c);
    return result + "'";
}

void install_schema(streamfind::ProjectTableStore &tables) {
    tables.execute(
        "CREATE TABLE IF NOT EXISTS MASS_SPEC_CHROMATOGRAMS ("
        "analysis VARCHAR NOT NULL, "
        "index INTEGER NOT NULL DEFAULT 0, chromatogram_id VARCHAR NOT NULL, "
        "polarity INTEGER, precursor_mz DOUBLE, activation_ce DOUBLE, product_mz DOUBLE, "
        "rt DOUBLE NOT NULL, raw_intensity DOUBLE NOT NULL, baseline DOUBLE NOT NULL DEFAULT 0, "
        "intensity DOUBLE NOT NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "PRIMARY KEY(analysis, chromatogram_id, rt))");
    tables.execute("ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS index INTEGER");
    tables.execute("ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS polarity INTEGER");
    tables.execute("ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS precursor_mz DOUBLE");
    tables.execute("ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS activation_ce DOUBLE");
    tables.execute("ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS product_mz DOUBLE");
}

void ensure_table(streamfind::Project &project) {
    streamfind::ProjectTableStore::transaction(project, {"MASS_SPEC_CHROMATOGRAMS"},
        [](streamfind::ProjectTableStore &tables) { install_chromatograms_schema(tables); });
}

std::optional<std::string> str_cell(const std::string &value) { return value; }
std::optional<std::string> inum_cell(int value) { return std::to_string(value); }
// Non-finite doubles become SQL NULL (mirrors the NTA `dn()` appender helper).
std::optional<std::string> dnum_cell(float value) {
    return std::isfinite(value) ? std::optional<std::string>(std::to_string(static_cast<double>(value))) : std::nullopt;
}

std::vector<std::string> chromatogram_columns() {
    return {"analysis", "index", "chromatogram_id", "polarity",
            "precursor_mz", "activation_ce", "product_mz",
            "rt", "raw_intensity", "baseline", "intensity"};
}

std::vector<std::optional<std::string>> chromatogram_cells(
    const std::string &analysis,
    const ::mass_spec::reader::MASS_SPEC_CHROMATOGRAMS_HEADERS &headers, std::size_t i,
    float rt, float intensity) {
    return {
        str_cell(analysis), inum_cell(headers.index[i]), str_cell(headers.chromatogram_id[i]),
        inum_cell(headers.polarity[i]),
        dnum_cell(headers.precursor_mz[i]), dnum_cell(headers.activation_ce[i]), dnum_cell(headers.product_mz[i]),
        std::to_string(rt), std::to_string(intensity), std::string("0"), std::to_string(intensity)};
}

std::vector<std::tuple<std::string, std::string, int>> analyses(
    streamfind::Project &project, const Json &parameters) {
    const auto rows = project.query_json(
        "SELECT analysis, file_path, analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis");
    const auto wanted = parameters.value("analysis_names", Json::array());
    std::vector<std::tuple<std::string, std::string, int>> result;
    for (const auto &row : rows) {
        const auto name = row.at("analysis").get<std::string>();
        bool selected = wanted.empty();
        for (const auto &value : wanted)
            selected = selected || value.get<std::string>() == name;
        if (selected) {
            int index = 0;
            if (auto it = row.find("analysis_index"); it != row.end() && !it->is_null()) {
                const auto text = it->get<std::string>();
                index = text.empty() ? 0 : std::stoi(text);
            }
            result.emplace_back(name, row.at("file_path").get<std::string>(), index);
        }
    }
    if (result.empty()) throw Error(ErrorCode::InvalidArgument, "No analyses available for chromatogram processing.");
    return result;
}

bool matches(const std::string &value, const Json &parameters) {
    const auto patterns = parameters.value("chromatogram_id_regex", Json::array());
    const auto flags = std::regex::ECMAScript |
                       (parameters.value("ignore_case", true) ? std::regex::icase : std::regex_constants::syntax_option_type{});
    for (const auto &pattern : patterns) {
        try {
            if (std::regex_search(value, std::regex(pattern.get<std::string>(), flags))) return true;
        } catch (const std::regex_error &) {
        }
    }
    return false;
}

Json status(const char *message) {
    return Json{{"status", "finished"}, {"info", message}};
}

}

void install_chromatograms_schema(streamfind::ProjectTableStore &tables) {
    detail::install_schema(tables);
}

Json load_chromatograms(streamfind::Project &project, const Json &parameters) {
    detail::ensure_table(project);
    for (const auto &[analysis, path, index] : detail::analyses(project, parameters)) {
        ::mass_spec::reader::MASS_SPEC_FILE file(path);
        file.select_analysis(index);
        const auto headers = file.get_chromatograms_headers();
        const auto arrays = file.get_chromatograms();
        std::vector<std::vector<std::optional<std::string>>> rows;
        for (std::size_t i = 0; i < arrays.size() && i < headers.chromatogram_id.size(); ++i) {
            const auto &id = headers.chromatogram_id[i];
            const bool keep = detail::matches(id, parameters) ^ parameters.value("invert", false);
            if (!keep || arrays[i].size() < 2) continue;
            project.execute_sql("DELETE FROM MASS_SPEC_CHROMATOGRAMS WHERE analysis = " + detail::sql(analysis) + " AND chromatogram_id = " + detail::sql(id));
            const auto &times = arrays[i][0];
            const auto &intensities = arrays[i][1];
            const auto count = std::min(times.size(), intensities.size());
            rows.reserve(rows.size() + count);
            for (std::size_t j = 0; j < count; ++j)
                rows.push_back(detail::chromatogram_cells(analysis, headers, i, times[j], intensities[j]));
        }
        if (!rows.empty())
            project.append_rows("MASS_SPEC_CHROMATOGRAMS", detail::chromatogram_columns(), rows);
    }
    return detail::status("Chromatograms loaded.");
}

Json filter_chromatograms_retention_time(streamfind::Project &project, const Json &parameters) {
    const double minimum = parameters.at("rt_min").get<double>();
    const double maximum = parameters.at("rt_max").get<double>();
    if (minimum >= maximum) throw Error(ErrorCode::InvalidArgument, "rt_min must be less than rt_max.");

    detail::ensure_table(project);
    std::string filter = " AND (rt < " + std::to_string(minimum) + " OR rt > " + std::to_string(maximum) + ")";
    const auto wanted = parameters.value("analysis_names", Json::array());
    if (!wanted.empty()) {
        filter += " AND analysis IN (";
        for (std::size_t i = 0; i < wanted.size(); ++i) filter += (i ? "," : "") + detail::sql(wanted[i].get<std::string>());
        filter += ")";
    }
    project.execute_sql("DELETE FROM MASS_SPEC_CHROMATOGRAMS WHERE 1=1" + filter);
    return detail::status("Chromatograms filtered by retention time.");
}

}
