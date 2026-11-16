#include "operations/chromatograms/operations.hpp"

#include "operations/base.hpp"
#include "readers/reader.hpp"
#include "utils/chromatograms.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace streamfind::mass_spec::chromatograms
{
    using Json = nlohmann::json;
    using utils::ChromatogramData;
    using utils::Peak;

    namespace
    {
        const std::vector<std::string> point_columns = {
            "analysis", "index", "chromatogram_id", "polarity", "precursor_mz", "activation_ce",
            "product_mz", "signal_type", "chromatogram_type", "detector", "channel", "wavelength_nm", "units",
            "rt", "raw_intensity", "baseline", "intensity"};
        const std::vector<std::string> point_types = {
            "string", "integer", "string", "integer", "real", "real", "real", "string", "string", "string",
            "string", "real", "string", "real", "real", "real", "real"};
        const std::vector<std::string> peak_columns = {
            "analysis", "chromatogram_index", "peak_id", "rt", "rt_start", "rt_end",
            "height", "raw_height", "area", "raw_area", "baseline_area", "width", "fwhm",
            "noise", "snr", "asymmetry", "sharpness", "plates", "number_points", "integration_algorithm",
            "integration_status", "manual_override", "chromatogram_id", "polarity", "precursor_mz",
            "product_mz", "activation_ce", "signal_type", "chromatogram_type", "detector", "channel",
            "wavelength_nm", "units", "eic_size", "eic_rt", "eic_intensity", "eic_raw_intensity",
            "eic_baseline"};
        const std::vector<std::string> peak_types = {
            "string",  // analysis
            "integer", // chromatogram_index
            "integer", // peak_id
            "real",    // rt
            "real",    // rt_start
            "real",    // rt_end
            "real",    // height
            "real",    // raw_height
            "real",    // area
            "real",    // raw_area
            "real",    // baseline_area
            "real",    // width
            "real",    // fwhm
            "real",    // noise
            "real",    // snr
            "real",    // asymmetry
            "real",    // sharpness
            "real",    // plates
            "integer", // number_points
            "string",  // integration_algorithm
            "string",  // integration_status
            "boolean", // manual_override
            "string",  // chromatogram_id
            "integer", // polarity
            "real",    // precursor_mz
            "real",    // product_mz
            "real",    // activation_ce
            "string",  // signal_type
            "string",  // chromatogram_type
            "string",  // detector
            "string",  // channel
            "real",    // wavelength_nm
            "string",  // units
            "integer", // eic_size
            "array",   // eic_rt
            "array",   // eic_intensity
            "array",   // eic_raw_intensity
            "array"};   // eic_baseline

        Json analysis_name_parameters(const Json &parameters, const Json &analyses)
        {
            auto result = parameters;
            const auto wanted = parameters.value("analysis_names", Json::array());
            Json names = Json::array();
            for (const auto &value : wanted)
            {
                if (value.is_string()) names.push_back(value);
                else if (value.is_number_integer() && value.get<std::size_t>() < analyses.size())
                    names.push_back(analyses[value.get<std::size_t>()].value("analysis", std::string{}));
            }
            result["analysis_names"] = std::move(names);
            return result;
        }

        bool selected(const Json &parameters, const Json &row)
        {
            if (!utils::selected_analysis(parameters, row))
                return false;
            return utils::selected_index(parameters, utils::integer(row, "index"));
        }

        Json point_rows(sdk::PluginProjectAccess &access, const Json &parameters)
        {
            return base::utils::input_rows(access, parameters, "chromatogramsTable", point_columns, "analysis");
        }

        void emit_points(sdk::PluginProjectAccess &access, Json rows)
        {
            access.emit_table_rows("chromatogramsTable", point_columns, point_types, rows);
        }

        Json point_row(const Json &source)
        {
            Json row;
            for (const auto &column : point_columns)
                if (source.contains(column))
                    row[column] = source.at(column);
            return row;
        }

        Json load_headers(const Json &parameters, sdk::PluginProjectAccess &access)
        {
            const auto analysis_rows = base::utils::input_rows(
                access, parameters, "analysesTable", {"analysis", "file_path", "analysis_index"}, "analysis");
            const auto header_rows = base::utils::input_rows(
                access, parameters, "chromatogramsHeadersTable", utils::header_columns(), "analysis");
            const auto header_map = utils::make_header_map(header_rows);
            std::map<std::string, std::vector<Json>> headers_by_analysis;
            for (const auto &header : header_rows)
                headers_by_analysis[utils::text(header, "analysis")].push_back(header);
            Json points = Json::array();
            std::size_t selected_chromatograms = 0;
            const auto wanted = parameters.value("analysis_names", Json::array());
            const auto patterns = parameters.value("chromatogram_id_regex", Json::array());
            const bool invert = parameters.value("invert", false);
            const bool ignore_case = parameters.value("ignore_case", true);

            for (const auto &analysis_row : analysis_rows)
            {
                const auto analysis = utils::text(analysis_row, "analysis");
                if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), analysis) == wanted.end())
                    continue;
                ::mass_spec::reader::MASS_SPEC_FILE file(utils::text(analysis_row, "file_path"));
                file.select_analysis(utils::integer(analysis_row, "analysis_index"));
                std::vector<int> selected_indices;
                std::vector<Json> selected_headers;
                const auto header_it = headers_by_analysis.find(analysis);
                if (header_it == headers_by_analysis.end())
                    continue;
                for (const auto &header : header_it->second)
                {
                    bool match = patterns.empty();
                    for (const auto &pattern : patterns)
                    {
                        try
                        {
                            auto flags = std::regex::ECMAScript;
                            if (ignore_case)
                                flags |= std::regex::icase;
                            if (std::regex_search(utils::text(header, "chromatogram_id"), std::regex(pattern.get<std::string>(), flags)))
                                match = true;
                        }
                        catch (const std::regex_error &)
                        {
                            throw std::invalid_argument("Invalid chromatogram_id_regex pattern");
                        }
                    }
                    if (invert)
                        match = !match;
                    if (!match)
                        continue;
                    ++selected_chromatograms;
                    selected_indices.push_back(utils::integer(header, "index"));
                    selected_headers.push_back(header);
                }
                const auto arrays = file.get_chromatograms(selected_indices);
                for (std::size_t selected = 0; selected < selected_headers.size(); ++selected)
                {
                    if (selected >= arrays.size() || arrays[selected].size() < 2)
                        continue;
                    const auto count = std::min(arrays[selected][0].size(), arrays[selected][1].size());
                    for (std::size_t j = 0; j < count; ++j)
                    {
                        const auto index = utils::integer(selected_headers[selected], "index");
                        Json point{{"analysis", analysis}, {"index", index}, {"rt", arrays[selected][0][j]},
                                   {"raw_intensity", arrays[selected][1][j]}, {"baseline", 0.0},
                                   {"intensity", arrays[selected][1][j]}};
                        const auto header = header_map.find({analysis, index});
                        if (header != header_map.end())
                            utils::append_header_fields(point, header->second);
                        points.push_back(std::move(point));
                    }
                }
            }
            emit_points(access, std::move(points));
            return Json{{"status", "finished"}, {"chromatograms", selected_chromatograms}};
        }

        Json transform_points(sdk::PluginProjectAccess &access, const Json &parameters,
                              const std::function<void(Json &)> &transform)
        {
            auto rows = point_rows(access, parameters);
            for (auto &row : rows)
                if (utils::selected_analysis(parameters, row))
                    transform(row);
            emit_points(access, std::move(rows));
            return Json{{"status", "finished"}};
        }

        std::map<std::pair<std::string, int>, ChromatogramData> groups_from(const Json &rows)
        {
            std::map<std::pair<std::string, int>, ChromatogramData> groups;
            for (const auto &row : rows)
            {
                auto &group = groups[{utils::text(row, "analysis"), utils::integer(row, "index")}];
                group.chromatogram_index = utils::integer(row, "index");
                group.rt.push_back(utils::real(row, "rt"));
                group.raw_intensity.push_back(utils::real(row, "raw_intensity"));
                group.baseline.push_back(utils::real(row, "baseline"));
                group.intensity.push_back(utils::real(row, "intensity"));
            }
            return groups;
        }

        Json nonzero_chromatogram_rows(Json rows)
        {
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Json &row)
                                      { return utils::real(row, "raw_intensity") <= 0.0; }),
                       rows.end());
            return rows;
        }
    }

    static std::vector<Peak> deduplicate_peaks(std::vector<Peak> peaks)
    {
        // A vendor trace can contain repeated samples at the same retention
        // time, and the detector may report the derivative and local-maximum
        // candidates separately.  Quantification must see one peak per
        // chromatogram/apex, otherwise an otherwise valid transition becomes
        // ambiguous.  The tolerance is much smaller than the SCIEX sampling
        // interval, so it cannot merge two independently sampled peaks.
        constexpr double retention_time_tolerance = 1.0e-3;
        std::sort(peaks.begin(), peaks.end(), [](const Peak &left, const Peak &right) {
            if (left.rt != right.rt) return left.rt < right.rt;
            if (left.area != right.area) return left.area > right.area;
            return left.raw_area > right.raw_area;
        });
        std::vector<Peak> unique;
        unique.reserve(peaks.size());
        for (const auto &peak : peaks)
        {
            if (!unique.empty() && std::abs(peak.rt - unique.back().rt) <= retention_time_tolerance)
            {
                if (peak.area > unique.back().area ||
                    (peak.area == unique.back().area && peak.raw_area > unique.back().raw_area))
                    unique.back() = peak;
                continue;
            }
            unique.push_back(peak);
        }
        return unique;
    }

    Json load_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return load_headers(parameters, access);
    }

    Json filter_chromatograms_retention_time(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const double minimum = parameters.at("rt_min").get<double>();
        const double maximum = parameters.at("rt_max").get<double>();
        if (minimum >= maximum)
            throw std::invalid_argument("rt_min must be less than rt_max");
        auto rows = point_rows(access, parameters);
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Json &row)
                                  { return utils::selected_analysis(parameters, row) &&
                                           (utils::real(row, "rt") < minimum || utils::real(row, "rt") > maximum); }),
                   rows.end());
        emit_points(access, std::move(rows));
        return Json{{"status", "finished"}};
    }

    Json correct_chromatogram_baseline(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto algorithm = parameters.value("baseline_algorithm", std::string("rolling_min"));
        const int window = parameters.value("window_size", 50);
        const double lambda = parameters.value("lambda", 1.0);
        const double asymmetry = parameters.value("asymmetry_penalty", 0.01);
        const int iterations = parameters.value("max_iterations", 10);
        if (algorithm != "none" && algorithm != "rolling_min" && algorithm != "als" && algorithm != "moving_average")
            throw std::invalid_argument("Unknown baseline algorithm: " + algorithm);
        auto rows = point_rows(access, parameters);
        const auto groups = groups_from(rows);
        for (const auto &[key, group] : groups)
        {
            if (!utils::selected_analysis(parameters, Json{{"analysis", key.first}}))
                continue;
            std::vector<double> baseline(group.intensity.size(), 0.0);
            if (algorithm == "rolling_min")
                baseline = utils::rolling_min_baseline(group.intensity, window);
            else if (algorithm == "moving_average")
                baseline = utils::moving_average_smooth(group.intensity, window);
            else if (algorithm == "als")
                baseline = utils::als_baseline(group.intensity, lambda, asymmetry, iterations);
            std::size_t offset = 0;
            for (auto &row : rows)
            {
                if (utils::text(row, "analysis") != key.first || utils::integer(row, "index") != key.second)
                    continue;
                if (offset >= baseline.size())
                    break;
                row["baseline"] = baseline[offset];
                row["intensity"] = std::max(0.0, utils::real(row, "intensity") - baseline[offset]);
                ++offset;
            }
        }
        emit_points(access, std::move(rows));
        return Json{{"status", "finished"}, {"algorithm", algorithm}};
    }

    Json smooth_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto algorithm = parameters.value("smoothing_algorithm", std::string("savitzky_golay"));
        const int window = parameters.value("window_size", 11);
        const int poly_order = parameters.value("poly_order", 2);
        if (algorithm != "none" && algorithm != "moving_average" && algorithm != "savitzky_golay")
            throw std::invalid_argument("Unknown smoothing algorithm: " + algorithm);
        auto rows = point_rows(access, parameters);
        const auto groups = groups_from(rows);
        for (const auto &[key, group] : groups)
        {
            if (!utils::selected_analysis(parameters, Json{{"analysis", key.first}}))
                continue;
            std::vector<double> smoothed = group.intensity;
            if (algorithm == "moving_average")
                smoothed = utils::moving_average_smooth(group.intensity, window);
            else if (algorithm == "savitzky_golay")
            {
                std::vector<double> d1, d2;
                int sg_window = std::min(window, static_cast<int>(group.intensity.size()) | 1);
                if (sg_window < 5)
                    sg_window = 5;
                if (sg_window % 2 == 0)
                    ++sg_window;
                utils::savitzky_golay_smooth(group.intensity, smoothed, d1, d2, sg_window, poly_order);
            }
            std::size_t offset = 0;
            for (auto &row : rows)
            {
                if (utils::text(row, "analysis") != key.first || utils::integer(row, "index") != key.second)
                    continue;
                if (offset >= smoothed.size())
                    break;
                row["intensity"] = smoothed[offset++];
            }
        }
        emit_points(access, std::move(rows));
        return Json{{"status", "finished"}, {"algorithm", algorithm}};
    }

    Json find_chromatogram_peaks(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = nonzero_chromatogram_rows(point_rows(access, parameters));
        const auto header_map = utils::make_header_map(rows);
        const auto groups = groups_from(rows);
        const bool merge = parameters.value("merge", true);
        const double merge_distance = parameters.value("merge_distance", 45.0);
        const double min_height = parameters.value("min_peak_height", 0.0);
        const double min_distance = parameters.value("min_peak_distance", 10.0);
        const double min_width = parameters.value("min_peak_width", 5.0);
        const double max_width = parameters.value("max_peak_width", 120.0);
        const double min_snr = parameters.value("min_snr", 10.0);
        const double trace_padding = parameters.value("peak_trace_padding_seconds", 0.0);
        if (trace_padding < 0.0)
            throw std::invalid_argument("peak_trace_padding_seconds must be non-negative");
        Json output = Json::array();
        int peak_id = 0;
        for (const auto &[key, group] : groups)
        {
            if (!utils::selected_analysis(parameters, Json{{"analysis", key.first}}) ||
                !utils::selected_index(parameters, key.second))
                continue;
            auto peaks = deduplicate_peaks(
                utils::find_peaks(group, merge, merge_distance, min_height, min_distance, min_width, max_width, min_snr));
            for (auto &peak : peaks)
            {
                peak.peak_id = ++peak_id;
                Json row{{"analysis", key.first}, {"chromatogram_index", key.second}, {"peak_id", peak.peak_id}, {"rt", peak.rt}, {"rt_start", peak.rt_start}, {"rt_end", peak.rt_end}, {"height", peak.height}, {"raw_height", peak.raw_height}, {"area", peak.area}, {"raw_area", peak.raw_area}, {"baseline_area", peak.baseline_area}, {"width", peak.width}, {"fwhm", peak.fwhm}, {"noise", peak.noise}, {"snr", peak.snr}, {"asymmetry", peak.asymmetry}, {"sharpness", peak.sharpness}, {"plates", peak.plates}, {"number_points", peak.number_points}, {"integration_algorithm", "streamfind.integration.v1"}, {"integration_status", "integrated"}, {"manual_override", false}};
                const auto header = header_map.find(key);
                if (header != header_map.end())
                    utils::append_header_fields(row, header->second);
                const auto trace_start = peak.rt_start - trace_padding;
                const auto trace_end = peak.rt_end + trace_padding;
                Json eic_rt = Json::array();
                Json eic_intensity = Json::array();
                Json eic_raw_intensity = Json::array();
                Json eic_baseline = Json::array();
                const auto count = std::min({group.rt.size(), group.intensity.size(), group.raw_intensity.size(), group.baseline.size()});
                for (std::size_t point = 0; point < count; ++point)
                {
                    if (group.rt[point] < trace_start || group.rt[point] > trace_end) continue;
                    eic_rt.push_back(group.rt[point]);
                    eic_intensity.push_back(group.intensity[point]);
                    eic_raw_intensity.push_back(group.raw_intensity[point]);
                    eic_baseline.push_back(group.baseline[point]);
                }
                row["eic_size"] = eic_rt.size();
                row["eic_rt"] = std::move(eic_rt);
                row["eic_intensity"] = std::move(eic_intensity);
                row["eic_raw_intensity"] = std::move(eic_raw_intensity);
                row["eic_baseline"] = std::move(eic_baseline);
                output.push_back(std::move(row));
            }
        }
        access.emit_table_rows("chromatogramPeaksTable", peak_columns, peak_types, output);
        return Json{{"status", "finished"}, {"peaks", output.size()}};
    }

    Json get_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto points = base::utils::input_rows(access, parameters, "chromatogramsTable", utils::point_columns(), "analysis");
        Json output = Json::array();
        for (const auto &point : points)
        {
            if (!utils::selected_analysis(parameters, point) ||
                !utils::selected_index(parameters, utils::integer(point, "index")))
                continue;
            output.push_back(point);
        }
        return output;
    }

    Json get_chromatogram_peaks(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto analyses = base::utils::input_rows(access, parameters, "analysesTable", {"analysis", "replicate"}, "analysis");
        std::map<std::string, std::string> replicates;
        for (const auto &analysis : analyses)
            replicates[utils::text(analysis, "analysis")] = utils::text(analysis, "replicate");
        const auto selection_parameters = analysis_name_parameters(parameters, analyses);
        const auto peaks = base::utils::input_rows(access, parameters, "chromatogramPeaksTable", utils::peak_columns(), "analysis");
        Json output = Json::array();
        for (const auto &peak : peaks)
        {
            if (!utils::selected_analysis(selection_parameters, peak) || !utils::selected_index(parameters, utils::integer(peak, "chromatogram_index")))
                continue;
            Json row = peak;
            row["replicate"] = replicates[utils::text(peak, "analysis")];
            output.push_back(std::move(row));
        }
        return output;
    }
}
