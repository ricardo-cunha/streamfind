#include "operations/chromatograms/operations.hpp"

#include "operations/base.hpp"
#include "readers/reader.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"
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
        const std::vector<std::string> peak_types = {
            "string", "integer", "integer", "string", "integer", "real", "real", "real", "string", "string",
            "string", "string", "real", "string", "real", "real", "real", "real", "real", "real", "real",
            "real", "real", "real", "real", "real", "real", "real", "real", "integer", "integer", "array",
            "array", "array", "array", "string", "string", "boolean"};

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

        sdk::TableBatchBuilder point_builder()
        {
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(point_columns.size());
            for (std::size_t index = 0; index < point_columns.size(); ++index)
            {
                auto type = STREAMFIND_PLUGIN_COLUMN_UTF8;
                if (point_types[index] == "integer") type = STREAMFIND_PLUGIN_COLUMN_INT64;
                else if (point_types[index] == "real") type = STREAMFIND_PLUGIN_COLUMN_FLOAT64;
                schema.push_back({point_columns[index], type});
            }
            return sdk::TableBatchBuilder(std::move(schema));
        }

        using PointKey = std::pair<std::string, int>;
        using PointGroups = std::map<PointKey, std::vector<double>>;

        bool selected_analysis_name(const Json &parameters, const std::string &name, std::size_t index)
        {
            const auto wanted = parameters.value("analysis_names", Json::array());
            if (wanted.empty()) return true;
            for (const auto &value : wanted)
                if ((value.is_string() && value.get<std::string>() == name) ||
                    (value.is_number_integer() && value.get<std::size_t>() == index)) return true;
            return false;
        }

        PointGroups read_point_groups(sdk::PluginProjectAccess &access, const Json &parameters)
        {
            PointGroups groups;
            std::map<std::string, std::size_t> analysis_indices;
            std::size_t next_analysis_index = 0;
            access.read_typed_batches(base::utils::input_table(parameters, "chromatogramsTable"), point_columns,
                                      "analysis", [&](const sdk::PluginProjectAccess::BatchView &batch) {
                const auto analysis_col = batch.column_index("analysis");
                const auto index_col = batch.column_index("index");
                const auto intensity_col = batch.column_index("intensity");
                for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                {
                    const auto analysis = batch.is_null(analysis_col, row)
                        ? std::string{} : std::string(batch.text(analysis_col, row));
                    auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                    if (inserted) ++next_analysis_index;
                    if (!selected_analysis_name(parameters, analysis, analysis_it->second)) continue;
                    const auto chromatogram_index = batch.is_null(index_col, row)
                        ? 0 : static_cast<int>(batch.integer(index_col, row));
                    const auto key = PointKey{analysis, chromatogram_index};
                    auto &intensities = groups[key];
                    intensities.push_back(batch.is_null(intensity_col, row) ? 0.0 : batch.real(intensity_col, row));
                }
            });
            return groups;
        }

        struct PeakInputs
        {
            std::map<PointKey, ChromatogramData> groups;
            utils::HeaderMap headers;
        };

        PeakInputs read_peak_inputs(sdk::PluginProjectAccess &access, const Json &parameters)
        {
            PeakInputs result;
            std::map<std::string, std::size_t> analysis_indices;
            std::size_t next_analysis_index = 0;
            access.read_typed_batches(base::utils::input_table(parameters, "chromatogramsTable"), point_columns,
                "analysis", [&](const sdk::PluginProjectAccess::BatchView &batch) {
                    const auto analysis_col = batch.column_index("analysis");
                    const auto index_col = batch.column_index("index");
                    const auto id_col = batch.column_index("chromatogram_id");
                    const auto polarity_col = batch.column_index("polarity");
                    const auto precursor_col = batch.column_index("precursor_mz");
                    const auto activation_col = batch.column_index("activation_ce");
                    const auto product_col = batch.column_index("product_mz");
                    const auto signal_col = batch.column_index("signal_type");
                    const auto type_col = batch.column_index("chromatogram_type");
                    const auto detector_col = batch.column_index("detector");
                    const auto channel_col = batch.column_index("channel");
                    const auto wavelength_col = batch.column_index("wavelength_nm");
                    const auto units_col = batch.column_index("units");
                    const auto rt_col = batch.column_index("rt");
                    const auto raw_col = batch.column_index("raw_intensity");
                    const auto baseline_col = batch.column_index("baseline");
                    const auto intensity_col = batch.column_index("intensity");
                    const auto string_at = [&](std::uint32_t col, std::uint64_t row) {
                        return batch.is_null(col, row) ? std::string{} : std::string(batch.text(col, row));
                    };
                    const auto real_at = [&](std::uint32_t col, std::uint64_t row) {
                        return batch.is_null(col, row) ? 0.0 : batch.real(col, row);
                    };
                    for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                    {
                        const auto analysis = string_at(analysis_col, row);
                        auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                        if (inserted) ++next_analysis_index;
                        if (!selected_analysis_name(parameters, analysis, analysis_it->second)) continue;
                        const auto index = batch.is_null(index_col, row) ? 0 : static_cast<int>(batch.integer(index_col, row));
                        const auto raw_intensity = real_at(raw_col, row);
                        if (raw_intensity <= 0.0) continue;
                        const PointKey key{analysis, index};
                        auto &group = result.groups[key];
                        group.chromatogram_index = index;
                        group.rt.push_back(real_at(rt_col, row));
                        group.raw_intensity.push_back(raw_intensity);
                        group.baseline.push_back(real_at(baseline_col, row));
                        group.intensity.push_back(real_at(intensity_col, row));
                        utils::HeaderInfo header;
                        header.chromatogram_id = string_at(id_col, row);
                        header.polarity = batch.is_null(polarity_col, row) ? 0 : static_cast<int>(batch.integer(polarity_col, row));
                        header.precursor_mz = utils::normalize_mz(real_at(precursor_col, row));
                        header.activation_ce = real_at(activation_col, row);
                        header.product_mz = utils::normalize_mz(real_at(product_col, row));
                        header.signal_type = string_at(signal_col, row);
                        header.chromatogram_type = string_at(type_col, row);
                        header.detector = string_at(detector_col, row);
                        header.channel = string_at(channel_col, row);
                        header.wavelength_nm = real_at(wavelength_col, row);
                        header.units = string_at(units_col, row);
                        result.headers.try_emplace(key, std::move(header));
                    }
                });
            return result;
        }

        void emit_adjusted_points(sdk::PluginProjectAccess &access, const Json &parameters,
                                  const std::map<PointKey, std::vector<double>> &replacement,
                                  const bool replace_baseline)
        {
            auto output = point_builder();
            std::map<std::string, std::size_t> analysis_indices;
            std::map<PointKey, std::size_t> offsets;
            std::size_t next_analysis_index = 0;
            access.read_typed_batches(base::utils::input_table(parameters, "chromatogramsTable"), point_columns,
                                      "analysis", [&](const sdk::PluginProjectAccess::BatchView &batch) {
                const auto analysis_col = batch.column_index("analysis");
                const auto index_col = batch.column_index("index");
                for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                {
                    const auto analysis = batch.is_null(analysis_col, row)
                        ? std::string{} : std::string(batch.text(analysis_col, row));
                    auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                    if (inserted) ++next_analysis_index;
                    const auto chromatogram_index = batch.is_null(index_col, row)
                        ? 0 : static_cast<int>(batch.integer(index_col, row));
                    const auto key = PointKey{analysis, chromatogram_index};
                    std::map<std::string, sdk::TableBatchBuilder::Cell> overrides;
                    const auto values = replacement.find(key);
                    if (values != replacement.end() && selected_analysis_name(parameters, analysis, analysis_it->second))
                    {
                        auto &offset = offsets[key];
                        if (offset < values->second.size())
                        {
                            if (replace_baseline) overrides["baseline"] = values->second[offset];
                            overrides["intensity"] = replace_baseline
                                ? std::max(0.0, (batch.is_null(batch.column_index("intensity"), row)
                                    ? 0.0 : batch.real(batch.column_index("intensity"), row)) - values->second[offset])
                                : values->second[offset];
                            ++offset;
                        }
                    }
                    output.append_row(batch, row, overrides);
                }
            });
            output.emit(access, "chromatogramsTable");
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
            const auto analysis_rows = base::utils::read_control_rows(
                access, parameters, "analysesTable", {"analysis", "file_path", "analysis_index"}, "analysis");
            const auto header_rows = base::utils::read_control_rows(
                access, parameters, "chromatogramsHeadersTable", utils::header_columns(), "analysis");
            const auto header_map = utils::make_header_map(header_rows);
            std::map<std::string, std::vector<Json>> headers_by_analysis;
            for (const auto &header : header_rows)
                headers_by_analysis[utils::text(header, "analysis")].push_back(header);
            auto points = point_builder();
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
                        const auto header = header_map.find({analysis, index});
                        if (header == header_map.end())
                            points.append_row({analysis, static_cast<std::int64_t>(index), std::monostate{},
                                std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{},
                                std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{},
                                std::monostate{}, std::monostate{}, arrays[selected][0][j],
                                arrays[selected][1][j], 0.0, arrays[selected][1][j]});
                        else
                        {
                            const auto &metadata = header->second;
                            points.append_row({analysis, static_cast<std::int64_t>(index), metadata.chromatogram_id,
                                static_cast<std::int64_t>(metadata.polarity), metadata.precursor_mz,
                                metadata.activation_ce, metadata.product_mz, metadata.signal_type,
                                metadata.chromatogram_type, metadata.detector, metadata.channel,
                                metadata.wavelength_nm, metadata.units, arrays[selected][0][j],
                                arrays[selected][1][j], 0.0, arrays[selected][1][j]});
                        }
                    }
                }
            }
            points.emit(access, "chromatogramsTable");
            return Json{{"status", "finished"}, {"chromatograms", selected_chromatograms}};
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
        auto output = point_builder();
        std::map<std::string, std::size_t> analysis_indices;
        std::size_t next_analysis_index = 0;
        access.read_typed_batches(base::utils::input_table(parameters, "chromatogramsTable"), point_columns,
            "analysis", [&](const sdk::PluginProjectAccess::BatchView &batch) {
                const auto analysis_col = batch.column_index("analysis");
                const auto rt_col = batch.column_index("rt");
                for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                {
                    const auto analysis = batch.is_null(analysis_col, row)
                        ? std::string{} : std::string(batch.text(analysis_col, row));
                    auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                    if (inserted) ++next_analysis_index;
                    const auto rt = batch.is_null(rt_col, row) ? 0.0 : batch.real(rt_col, row);
                    const bool keep = !selected_analysis_name(parameters, analysis, analysis_it->second) ||
                        (rt >= minimum && rt <= maximum);
                    if (keep) output.append_row(batch, row);
                }
            });
        output.emit(access, "chromatogramsTable");
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
        const auto groups = read_point_groups(access, parameters);
        std::map<PointKey, std::vector<double>> baselines;
        for (const auto &[key, group] : groups)
        {
            std::vector<double> baseline(group.size(), 0.0);
            if (algorithm == "rolling_min")
                baseline = utils::rolling_min_baseline(group, window);
            else if (algorithm == "moving_average")
                baseline = utils::moving_average_smooth(group, window);
            else if (algorithm == "als")
                baseline = utils::als_baseline(group, lambda, asymmetry, iterations);
            baselines.emplace(key, std::move(baseline));
        }
        emit_adjusted_points(access, parameters, baselines, true);
        return Json{{"status", "finished"}, {"algorithm", algorithm}};
    }

    Json smooth_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto algorithm = parameters.value("smoothing_algorithm", std::string("savitzky_golay"));
        const int window = parameters.value("window_size", 11);
        const int poly_order = parameters.value("poly_order", 2);
        if (algorithm != "none" && algorithm != "moving_average" && algorithm != "savitzky_golay")
            throw std::invalid_argument("Unknown smoothing algorithm: " + algorithm);
        const auto groups = read_point_groups(access, parameters);
        std::map<PointKey, std::vector<double>> smoothed_groups;
        for (const auto &[key, group] : groups)
        {
            std::vector<double> smoothed = group;
            if (algorithm == "moving_average")
                smoothed = utils::moving_average_smooth(group, window);
            else if (algorithm == "savitzky_golay")
            {
                std::vector<double> d1, d2;
                int sg_window = std::min(window, static_cast<int>(group.size()) | 1);
                if (sg_window < 5)
                    sg_window = 5;
                if (sg_window % 2 == 0)
                    ++sg_window;
                utils::savitzky_golay_smooth(group, smoothed, d1, d2, sg_window, poly_order);
            }
            smoothed_groups.emplace(key, std::move(smoothed));
        }
        emit_adjusted_points(access, parameters, smoothed_groups, false);
        return Json{{"status", "finished"}, {"algorithm", algorithm}};
    }

    Json find_chromatogram_peaks(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto &peak_columns = utils::peak_columns();
        if (peak_types.size() != peak_columns.size())
            throw std::logic_error("chromatogram peak output types do not match the canonical table schema");
        const auto inputs = read_peak_inputs(access, parameters);
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
        std::vector<sdk::TableBatchBuilder::Column> schema;
        schema.reserve(peak_columns.size());
        for (std::size_t index = 0; index < peak_columns.size(); ++index)
        {
            const auto &type = peak_types[index];
            if (type == "integer") schema.push_back({peak_columns[index], STREAMFIND_PLUGIN_COLUMN_INT64});
            else if (type == "real") schema.push_back({peak_columns[index], STREAMFIND_PLUGIN_COLUMN_FLOAT64});
            else if (type == "boolean") schema.push_back({peak_columns[index], STREAMFIND_PLUGIN_COLUMN_BOOL});
            else if (type == "array") schema.push_back({peak_columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8, true,
                                                          STREAMFIND_PLUGIN_COLUMN_FLOAT64});
            else schema.push_back({peak_columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8});
        }
        sdk::TableBatchBuilder output(std::move(schema));
        int peak_id = 0;
        for (const auto &[key, group] : inputs.groups)
        {
            if (!utils::selected_index(parameters, key.second))
                continue;
            auto peaks = deduplicate_peaks(
                utils::find_peaks(group, merge, merge_distance, min_height, min_distance, min_width, max_width, min_snr));
            for (auto &peak : peaks)
            {
                peak.peak_id = ++peak_id;
                const auto header = inputs.headers.find(key);
                const auto trace_start = peak.rt_start - trace_padding;
                const auto trace_end = peak.rt_end + trace_padding;
                std::vector<sdk::TableBatchBuilder::ListCell> eic_rt;
                std::vector<sdk::TableBatchBuilder::ListCell> eic_intensity;
                std::vector<sdk::TableBatchBuilder::ListCell> eic_raw_intensity;
                std::vector<sdk::TableBatchBuilder::ListCell> eic_baseline;
                const auto count = std::min({group.rt.size(), group.intensity.size(), group.raw_intensity.size(), group.baseline.size()});
                for (std::size_t point = 0; point < count; ++point)
                {
                    if (group.rt[point] < trace_start || group.rt[point] > trace_end) continue;
                    eic_rt.emplace_back(group.rt[point]);
                    eic_intensity.emplace_back(group.intensity[point]);
                    eic_raw_intensity.emplace_back(group.raw_intensity[point]);
                    eic_baseline.emplace_back(group.baseline[point]);
                }
                const auto nullable_string = [](const std::string &value) -> sdk::TableBatchBuilder::Cell {
                    return value.empty() ? sdk::TableBatchBuilder::Cell(std::monostate{})
                                         : sdk::TableBatchBuilder::Cell(value);
                };
                std::vector<sdk::TableBatchBuilder::Cell> values{
                    key.first, static_cast<std::int64_t>(key.second), static_cast<std::int64_t>(peak.peak_id)};
                if (header == inputs.headers.end())
                {
                    values.insert(values.end(), {std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{},
                        std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{},
                        std::monostate{}, std::monostate{}});
                }
                else
                {
                    const auto &metadata = header->second;
                    values.insert(values.end(), {metadata.chromatogram_id, static_cast<std::int64_t>(metadata.polarity),
                        metadata.precursor_mz, metadata.product_mz, metadata.activation_ce,
                        nullable_string(metadata.signal_type), nullable_string(metadata.chromatogram_type),
                        nullable_string(metadata.detector), nullable_string(metadata.channel), metadata.wavelength_nm,
                        nullable_string(metadata.units)});
                }
                values.insert(values.end(), {
                    peak.rt, peak.rt_start, peak.rt_end, peak.height, peak.raw_height, peak.area, peak.raw_area,
                    peak.baseline_area, peak.width, peak.fwhm, peak.noise, peak.snr, peak.asymmetry, peak.sharpness,
                    peak.plates, static_cast<std::int64_t>(peak.number_points)});
                values.emplace_back(static_cast<std::int64_t>(eic_rt.size()));
                values.emplace_back(std::move(eic_rt));
                values.emplace_back(std::move(eic_intensity));
                values.emplace_back(std::move(eic_raw_intensity));
                values.emplace_back(std::move(eic_baseline));
                values.emplace_back(std::string("streamfind.integration.v1"));
                values.emplace_back(std::string("integrated"));
                values.emplace_back(false);
                output.append_row(values);
            }
        }
        const auto peak_count = output.row_count();
        output.emit(access, "chromatogramPeaksTable");
        return Json{{"status", "finished"}, {"peaks", peak_count}};
    }

    Json get_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto table = base::utils::input_table(parameters, "chromatogramsTable");
        const auto indices = parameters.value("indices", Json::array());
        std::map<std::string, std::size_t> analysis_indices;
        std::size_t next_analysis_index = 0;
        std::size_t emitted = 0;
        access.read_typed_batches(table, point_columns, "analysis", [&](const auto &batch) {
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(point_columns.size());
            for (std::uint32_t column = 0; column < batch.column_count(); ++column)
                schema.push_back({point_columns.at(column), batch.type(column), batch.is_list(column),
                    batch.is_list(column) ? batch.list_child_type(column) : STREAMFIND_PLUGIN_COLUMN_UTF8});
            sdk::TableBatchBuilder output(std::move(schema));
            const auto analysis_col = batch.column_index("analysis");
            const auto index_col = batch.column_index("index");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                const auto analysis = batch.is_null(analysis_col, row)
                    ? std::string{} : std::string(batch.text(analysis_col, row));
                auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                if (inserted) ++next_analysis_index;
                if (!selected_analysis_name(parameters, analysis, analysis_it->second)) continue;
                const auto chromatogram_index = batch.is_null(index_col, row)
                    ? 0 : static_cast<int>(batch.integer(index_col, row));
                if (!indices.empty() && !utils::selected_index(parameters, chromatogram_index)) continue;
                output.append_row(batch, row);
                if (output.row_count() >= 1024) {
                    emitted += static_cast<std::size_t>(output.row_count());
                    output.emit(access, "chromatogramsTable");
                    output.clear();
                }
            }
            if (output.row_count() != 0) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, "chromatogramsTable");
            }
        });
        return {{"status", "finished"}, {"rows", emitted}};
    }

    Json get_chromatogram_peaks(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto table = base::utils::input_table(parameters, "chromatogramPeaksTable");
        const auto indices = parameters.value("indices", Json::array());
        std::map<std::string, std::size_t> analysis_indices;
        std::size_t next_analysis_index = 0;
        std::size_t emitted = 0;
        access.read_typed_batches(table, utils::peak_columns(), "analysis", [&](const auto &batch) {
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(utils::peak_columns().size());
            for (std::uint32_t column = 0; column < batch.column_count(); ++column)
                schema.push_back({utils::peak_columns().at(column), batch.type(column), batch.is_list(column),
                    batch.is_list(column) ? batch.list_child_type(column) : STREAMFIND_PLUGIN_COLUMN_UTF8});
            sdk::TableBatchBuilder output(std::move(schema));
            const auto analysis_col = batch.column_index("analysis");
            const auto index_col = batch.column_index("chromatogram_index");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                const auto analysis = batch.is_null(analysis_col, row)
                    ? std::string{} : std::string(batch.text(analysis_col, row));
                auto [analysis_it, inserted] = analysis_indices.emplace(analysis, next_analysis_index);
                if (inserted) ++next_analysis_index;
                if (!selected_analysis_name(parameters, analysis, analysis_it->second)) continue;
                const auto chromatogram_index = batch.is_null(index_col, row)
                    ? 0 : static_cast<int>(batch.integer(index_col, row));
                if (!indices.empty() && !utils::selected_index(parameters, chromatogram_index)) continue;
                output.append_row(batch, row);
                if (output.row_count() >= 1024) {
                    emitted += static_cast<std::size_t>(output.row_count());
                    output.emit(access, "chromatogramPeaksTable");
                    output.clear();
                }
            }
            if (output.row_count() != 0) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, "chromatogramPeaksTable");
            }
        });
        return {{"status", "finished"}, {"rows", emitted}};
    }
}
