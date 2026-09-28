#include "operations/chromatograms/operations.hpp"

#include "operations/base.hpp"
#include "readers/reader.hpp"
#include "utils/chromatograms.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
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
            "analysis", "index", "rt", "raw_intensity", "baseline", "intensity", "created_at"};
        const std::vector<std::string> point_types = {
            "string", "integer", "real", "real", "real", "real", "timestamp"};
        const std::vector<std::string> peak_columns = {
            "analysis", "chromatogram_index", "peak_id", "rt", "rt_start", "rt_end",
            "height", "raw_height", "area", "raw_area", "baseline_area", "width", "fwhm",
            "snr", "asymmetry", "sharpness", "plates", "number_points", "integration_algorithm",
            "integration_status", "manual_override", "created_at"};
        const std::vector<std::string> peak_types = {
            "string", "integer", "integer", "real", "real", "real", "real", "real", "real",
            "real", "real", "real", "real", "real", "real", "real", "real", "integer",
            "string", "string", "boolean", "timestamp"};

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

        std::string utc_now()
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &time);
#else
            gmtime_r(&time, &tm);
#endif
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            return out.str();
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
            for (auto &row : rows)
                row["created_at"] = utc_now();
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
            const std::vector<std::string> columns = {
                "analysis", "index", "chromatogram_id", "array_length", "polarity", "precursor_mz",
                "activation_ce", "product_mz", "signal_type", "chromatogram_type", "detector", "channel",
                "units", "wavelength_nm", "interval_ms", "start_time", "end_time", "intensity_multiplier"};
            const std::vector<std::string> types = {
                "string", "integer", "string", "integer", "integer", "real", "real", "real", "string",
                "string", "string", "string", "string", "real", "real", "real", "real", "real"};
            Json headers = Json::array();
            Json points = Json::array();
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
                const auto raw_headers = file.get_chromatograms_headers();
                const auto arrays = file.get_chromatograms();
                for (std::size_t i = 0; i < raw_headers.index.size(); ++i)
                {
                    bool match = patterns.empty();
                    for (const auto &pattern : patterns)
                    {
                        try
                        {
                            auto flags = std::regex::ECMAScript;
                            if (ignore_case)
                                flags |= std::regex::icase;
                            if (std::regex_search(raw_headers.chromatogram_id[i], std::regex(pattern.get<std::string>(), flags)))
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
                    Json header = {
                        {"analysis", analysis}, {"index", raw_headers.index[i]}, {"chromatogram_id", raw_headers.chromatogram_id[i]}, {"array_length", raw_headers.array_length[i]}, {"polarity", raw_headers.polarity[i]}, {"precursor_mz", raw_headers.precursor_mz[i]}, {"activation_ce", raw_headers.activation_ce[i]}, {"product_mz", raw_headers.product_mz[i]}, {"signal_type", raw_headers.signal_type[i]}, {"chromatogram_type", raw_headers.chromatogram_type[i]}, {"detector", raw_headers.detector[i]}, {"channel", raw_headers.channel[i]}, {"units", raw_headers.units[i]}, {"wavelength_nm", raw_headers.wavelength_nm[i]}, {"interval_ms", raw_headers.interval_ms[i]}, {"start_time", raw_headers.start_time[i]}, {"end_time", raw_headers.end_time[i]}, {"intensity_multiplier", raw_headers.intensity_multiplier[i]}};
                    headers.push_back(std::move(header));
                    if (i >= arrays.size() || arrays[i].size() < 2)
                        continue;
                    const auto count = std::min(arrays[i][0].size(), arrays[i][1].size());
                    for (std::size_t j = 0; j < count; ++j)
                        points.push_back({{"analysis", analysis}, {"index", raw_headers.index[i]}, {"rt", arrays[i][0][j]}, {"raw_intensity", arrays[i][1][j]}, {"baseline", 0.0}, {"intensity", arrays[i][1][j]}});
                }
            }
            access.emit_table_rows("chromatogramsHeadersTable", columns, types, headers);
            emit_points(access, std::move(points));
            return Json{{"status", "finished"}, {"chromatograms", headers.size()}};
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
                group.intensity.push_back(utils::real(row, "intensity"));
            }
            return groups;
        }
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
        const auto rows = point_rows(access, parameters);
        const auto groups = groups_from(rows);
        const bool merge = parameters.value("merge", true);
        const double merge_distance = parameters.value("merge_distance", 45.0);
        const double min_height = parameters.value("min_peak_height", 0.0);
        const double min_distance = parameters.value("min_peak_distance", 10.0);
        const double min_width = parameters.value("min_peak_width", 5.0);
        const double max_width = parameters.value("max_peak_width", 120.0);
        const double min_snr = parameters.value("min_snr", 10.0);
        Json output = Json::array();
        int peak_id = 0;
        for (const auto &[key, group] : groups)
        {
            if (!utils::selected_analysis(parameters, Json{{"analysis", key.first}}) ||
                !utils::selected_index(parameters, key.second))
                continue;
            auto peaks = utils::find_peaks(group, merge, merge_distance, min_height, min_distance, min_width, max_width, min_snr);
            for (auto &peak : peaks)
            {
                peak.peak_id = ++peak_id;
                output.push_back({{"analysis", key.first}, {"chromatogram_index", key.second}, {"peak_id", peak.peak_id}, {"rt", peak.rt}, {"rt_start", peak.rt_start}, {"rt_end", peak.rt_end}, {"height", peak.height}, {"raw_height", peak.raw_height}, {"area", peak.area}, {"raw_area", peak.raw_area}, {"baseline_area", peak.baseline_area}, {"width", peak.width}, {"fwhm", peak.fwhm}, {"snr", peak.snr}, {"asymmetry", peak.asymmetry}, {"sharpness", peak.sharpness}, {"plates", peak.plates}, {"number_points", peak.number_points}, {"integration_algorithm", "streamfind.integration.v1"}, {"integration_status", "integrated"}, {"manual_override", false}});
            }
        }
        for (auto &row : output)
            row["created_at"] = utc_now();
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
        const auto headers = base::utils::input_rows(access, parameters, "chromatogramsHeadersTable", utils::header_columns(), "analysis");
        const auto peaks = base::utils::input_rows(access, parameters, "chromatogramPeaksTable", utils::peak_columns(), "analysis");
        const auto header_map = utils::make_header_map(headers);
        Json output = Json::array();
        for (const auto &peak : peaks)
        {
            if (!utils::selected_analysis(selection_parameters, peak) || !utils::selected_index(parameters, utils::integer(peak, "chromatogram_index")))
                continue;
            Json row = peak;
            const auto it = header_map.find({utils::text(peak, "analysis"), utils::integer(peak, "chromatogram_index")});
            if (it != header_map.end())
                utils::append_header_fields(row, it->second);
            row["replicate"] = replicates[utils::text(peak, "analysis")];
            output.push_back(std::move(row));
        }
        return output;
    }
}
