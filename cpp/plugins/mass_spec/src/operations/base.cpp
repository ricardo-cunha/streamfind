#include "operations/base.hpp"
#include "readers/reader.hpp"
#include "utils/base.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

#include <utility>
#include <vector>
#include <limits>

using Json = nlohmann::json;

namespace streamfind::mass_spec::base
{

    namespace
    {

        using Json = nlohmann::json;
        using StringRow = std::vector<std::optional<std::string>>;

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char character)
                           { return static_cast<char>(std::tolower(character)); });
            return value;
        }

        std::string utc_now()
        {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            char buffer[20]{};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc);
            return buffer;
        }

        void harmonize_chromatogram_ids(::mass_spec::reader::MASS_SPEC_CHROMATOGRAMS_HEADERS &headers)
        {
            for (std::size_t index = 0; index < headers.chromatogram_id.size(); ++index)
            {
                if (headers.chromatogram_id[index].empty())
                    headers.chromatogram_id[index] = "chromatogram_" + std::to_string(index);
            }
        }

        Json typed_rows(const std::vector<StringRow> &source,
                        const std::vector<std::string> &names,
                        const std::vector<std::string> &types)
        {
            Json result = Json::array();
            for (const auto &source_row : source)
            {
                Json row = Json::object();
                for (std::size_t index = 0; index < names.size(); ++index)
                {
                    if (!source_row[index].has_value())
                    {
                        row[names[index]] = nullptr;
                        continue;
                    }
                    const auto &value = *source_row[index];
                    if (types[index] == "integer")
                        row[names[index]] = std::stoll(value);
                    else if (types[index] == "real")
                        row[names[index]] = std::stod(value);
                    else if (types[index] == "boolean")
                        row[names[index]] = value == "true";
                    else
                        row[names[index]] = value;
                }
                result.push_back(std::move(row));
            }
            return result;
        }

    } // namespace

    Json read_mass_spec_files(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto source_paths_it = parameters.find("source_paths");
        if (source_paths_it == parameters.end() || !source_paths_it->is_array())
            throw std::invalid_argument("source_paths must be an array of strings");
        std::vector<std::string> source_paths;
        source_paths.reserve(source_paths_it->size());
        for (const auto &source_path : *source_paths_it)
        {
            if (!source_path.is_string() || source_path.get<std::string>().empty())
                throw std::invalid_argument("source_paths must contain non-empty strings");
            source_paths.push_back(source_path.get<std::string>());
        }
        const std::vector<std::string> analysis_columns = {
            "analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
            "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
            "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
            "start_rt", "end_rt", "has_ion_mobility", "concentration", "created_at"};
        const std::vector<std::string> analysis_types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real", "timestamp"};
        const std::vector<std::string> spectra_columns = {
            "analysis", "index", "scan", "array_length", "level", "mode", "polarity", "configuration", "lowmz",
            "highmz", "bpmz", "bpint", "tic", "rt", "mobility", "window_mz", "window_mzlow", "window_mzhigh",
            "precursor_mz", "precursor_intensity", "precursor_charge", "activation_ce"};
        const std::vector<std::string> spectra_types = {
            "string", "integer", "integer", "integer", "integer", "integer", "integer", "integer", "real", "real",
            "real", "real", "real", "real", "real", "real", "real", "real", "real", "real", "integer", "real"};
        const std::vector<std::string> chromatogram_columns = {
            "analysis", "index", "chromatogram_id", "array_length", "polarity", "precursor_mz", "activation_ce",
            "product_mz", "signal_type", "chromatogram_type", "detector", "channel", "units", "wavelength_nm",
            "interval_ms", "start_time", "end_time", "intensity_multiplier"};
        const std::vector<std::string> chromatogram_types = {
            "string", "integer", "string", "integer", "integer", "real", "real", "real", "string", "string",
            "string", "string", "string", "real", "real", "real", "real", "real"};

        std::vector<StringRow> analysis_rows;
        std::vector<StringRow> spectra_rows;
        std::vector<StringRow> chromatogram_rows;
        Json added = Json::array();

        for (const auto &source_path_value : source_paths)
        {
            const std::filesystem::path path = source_path_value;
            ::mass_spec::reader::MASS_SPEC_FILE file(path.string());
            const std::string replicate;
            const std::string blank;
            const auto catalog = file.get_analysis_catalog();
            for (const auto &descriptor : catalog)
            {
                file.select_analysis(descriptor.analysis_index);
                const auto summary = file.get_summary();
                const auto analysis = catalog.size() == 1
                                          ? path.stem().string()
                                          : path.stem().string() + "_" + descriptor.name;
                analysis_rows.push_back({analysis, std::to_string(descriptor.analysis_index),
                                         std::to_string(descriptor.source_analysis_number),
                                         replicate, blank, path.filename().string(), path.string(), path.parent_path().string(),
                                         lower(path.extension().string()), summary.format, "MS", summary.time_stamp,
                                         std::to_string(summary.number_spectra), std::to_string(summary.number_chromatograms),
                                         std::to_string(summary.number_spectra_binary_arrays), std::to_string(summary.min_mz),
                                         std::to_string(summary.max_mz), std::to_string(summary.start_rt), std::to_string(summary.end_rt),
                                         summary.has_ion_mobility ? "true" : "false", std::nullopt, utc_now()});
                added.push_back({{"analysis", analysis}, {"file_path", path.string()}, {"analysis_index", descriptor.analysis_index}, {"source_analysis_number", descriptor.source_analysis_number}, {"replicate", replicate}, {"blank", blank}});

                const auto spectra = file.get_spectra_headers();
                for (std::size_t index = 0; index < spectra.index.size(); ++index)
                    spectra_rows.push_back({analysis, std::to_string(spectra.index[index]), std::to_string(spectra.scan[index]),
                                            std::to_string(spectra.array_length[index]), std::to_string(spectra.level[index]),
                                            std::to_string(spectra.mode[index]), std::to_string(spectra.polarity[index]),
                                            std::to_string(spectra.configuration[index]), std::to_string(spectra.lowmz[index]),
                                            std::to_string(spectra.highmz[index]), std::to_string(spectra.bpmz[index]),
                                            std::to_string(spectra.bpint[index]), std::to_string(spectra.tic[index]),
                                            std::to_string(spectra.rt[index]), std::to_string(spectra.mobility[index]),
                                            std::to_string(spectra.window_mz[index]), std::to_string(spectra.window_mzlow[index]),
                                            std::to_string(spectra.window_mzhigh[index]), std::to_string(spectra.precursor_mz[index]),
                                            std::to_string(spectra.precursor_intensity[index]), std::to_string(spectra.precursor_charge[index]),
                                            std::to_string(spectra.activation_ce[index])});

                auto chromatograms = file.get_chromatograms_headers();
                harmonize_chromatogram_ids(chromatograms);
                for (std::size_t index = 0; index < chromatograms.chromatogram_id.size(); ++index)
                    chromatogram_rows.push_back({analysis, std::to_string(chromatograms.index[index]),
                                                 chromatograms.chromatogram_id[index], std::to_string(chromatograms.array_length[index]),
                                                 std::to_string(chromatograms.polarity[index]), std::to_string(chromatograms.precursor_mz[index]),
                                                 std::to_string(chromatograms.activation_ce[index]), std::to_string(chromatograms.product_mz[index]),
                                                 chromatograms.signal_type[index], chromatograms.chromatogram_type[index], chromatograms.detector[index],
                                                 chromatograms.channel[index], chromatograms.units[index], std::to_string(chromatograms.wavelength_nm[index]),
                                                 std::to_string(chromatograms.interval_ms[index]), std::to_string(chromatograms.start_time[index]),
                                                 std::to_string(chromatograms.end_time[index]), std::to_string(chromatograms.intensity_multiplier[index])});
            }
        }

        if (!analysis_rows.empty())
            access.emit_table_rows("analysesTable", analysis_columns, analysis_types,
                                   typed_rows(analysis_rows, analysis_columns, analysis_types));
        if (!spectra_rows.empty())
            access.emit_table_rows("spectraHeadersTable", spectra_columns, spectra_types,
                                   typed_rows(spectra_rows, spectra_columns, spectra_types));
        if (!chromatogram_rows.empty())
            access.emit_table_rows("chromatogramsHeadersTable", chromatogram_columns, chromatogram_types,
                                   typed_rows(chromatogram_rows, chromatogram_columns, chromatogram_types));
        return added;
    }


    Json remove_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto &inputs = parameters.at("_inputs");
        const auto &analyses_input = inputs.at("analysesTable");
        const auto physical_table = analyses_input.at("physical_table").get<std::string>();
        const std::vector<std::string> columns = {
            "analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
            "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
            "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
            "start_rt", "end_rt", "has_ion_mobility", "concentration", "created_at"};
        const std::vector<std::string> types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real", "timestamp"};
        std::vector<std::regex> patterns;
        std::vector<std::string> literal_patterns;
        for (const auto &pattern_value : parameters.at("analysis_names"))
        {
            const auto pattern = pattern_value.get<std::string>();
            if (pattern.empty())
                throw std::invalid_argument("analysis_names regex patterns must not be empty");
            if (pattern.find_first_of(R"(\^$.*+?()[]{}|)") == std::string::npos)
            {
                literal_patterns.push_back(pattern);
                continue;
            }
            try
            {
                patterns.emplace_back(pattern, std::regex::ECMAScript);
            }
            catch (const std::regex_error &error)
            {
                throw std::invalid_argument("invalid analysis_names regex pattern: " + pattern + " (" + error.what() + ")");
            }
        }

        const auto source_rows = access.read(physical_table, columns, "analysis");
        std::vector<StringRow> retained_rows;
        Json removed = Json::array();
        for (const auto &source_row : source_rows)
        {
            const auto analysis = source_row.value("analysis", std::string{});
            const bool matches_literal = std::find(literal_patterns.begin(), literal_patterns.end(), analysis) != literal_patterns.end();
            const bool matches_regex = std::any_of(patterns.begin(), patterns.end(), [&](const auto &pattern) {
                return std::regex_search(analysis, pattern);
            });
            if (matches_literal || matches_regex)
            {
                removed.push_back(analysis);
                continue;
            }
            StringRow row;
            row.reserve(columns.size());
            for (const auto &column : columns)
            {
                if (source_row.at(column).is_null())
                    row.push_back(std::nullopt);
                else if (source_row.at(column).is_string())
                    row.push_back(source_row.at(column).get<std::string>());
                else
                    row.push_back(source_row.at(column).dump());
            }
            retained_rows.push_back(std::move(row));
        }
        access.emit_table_rows("analysesTable", columns, types,
                               typed_rows(retained_rows, columns, types));
        return removed;
    }

    namespace
    {


        const std::vector<std::string> analysis_columns = {
            "analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
            "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
            "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
            "start_rt", "end_rt", "has_ion_mobility", "concentration", "created_at"};
        const std::vector<std::string> analysis_types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real", "timestamp"};

        Json emit_analysis_copy(sdk::PluginProjectAccess &access, const Json &parameters,
                                const Json &source, const char *changed_key = nullptr,
                                const Json *changed_values = nullptr)
        {
            if (changed_values != nullptr && changed_values->size() != source.size())
                throw std::invalid_argument(std::string(changed_key) + " length must match analyses");
            Json output = Json::array();
            for (std::size_t row_index = 0; row_index < source.size(); ++row_index)
            {
                auto row = source[row_index];
                if (changed_key != nullptr)
                    row[changed_key] = (*changed_values)[row_index];
                for (std::size_t column_index = 0; column_index < analysis_columns.size(); ++column_index)
                {
                    const auto &column = analysis_columns[column_index];
                    if (!row.contains(column) || row[column].is_null() || !row[column].is_string())
                        continue;
                    const auto value = row[column].get<std::string>();
                    if (analysis_types[column_index] == "integer")
                        row[column] = std::stoll(value);
                    else if (analysis_types[column_index] == "real")
                        row[column] = std::stod(value);
                    else if (analysis_types[column_index] == "boolean")
                        row[column] = value == "true";
                }
                output.push_back(std::move(row));
            }
            access.emit_table_rows("analysesTable", analysis_columns, analysis_types, output);
            return Json{{"updated", output.size()}};
        }

        Json analysis_value_rows(sdk::PluginProjectAccess &access, const Json &parameters, const char *column,
                                 bool numeric)
        {
            const auto rows = base::utils::input_rows(access, parameters, "analysesTable", {column}, "analysis");
            Json result = Json::array();
            for (const auto &row : rows)
            {
                const auto value = row.value(column, std::string{});
                result.push_back(numeric && !value.empty() ? Json(std::stod(value)) : Json(value));
            }
            return result;
        }

        Json selected_analysis_rows(sdk::PluginProjectAccess &access, const Json &parameters)
        {
            return base::utils::input_rows(access, parameters, "analysesTable", analysis_columns, "analysis");
        }

        Json analysis_names(const Json &parameters)
        {
            return parameters.value("analysis_names", Json::array());
        }

        bool selected(const Json &wanted, const std::string &name)
        {
            return wanted.empty() || std::find(wanted.begin(), wanted.end(), name) != wanted.end();
        }

    } // namespace

    Json get_analyses_info(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return base::utils::input_rows(access, parameters, "analysesTable",
                          {"analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
                           "file_path", "format", "number_spectra", "number_chromatograms"},
                          "analysis");
    }

    Json get_analysis_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return analysis_value_rows(access, parameters, "analysis", false);
    }

    Json get_replicate_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return analysis_value_rows(access, parameters, "replicate", false);
    }

    Json get_blank_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return analysis_value_rows(access, parameters, "blank", false);
    }

    Json get_concentrations(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return analysis_value_rows(access, parameters, "concentration", true);
    }

    Json set_replicate_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = selected_analysis_rows(access, parameters);
        return emit_analysis_copy(access, parameters, rows, "replicate", &parameters.at("replicate_names"));
    }

    Json set_blank_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = selected_analysis_rows(access, parameters);
        return emit_analysis_copy(access, parameters, rows, "blank", &parameters.at("blank_names"));
    }

    Json set_concentrations(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = selected_analysis_rows(access, parameters);
        return emit_analysis_copy(access, parameters, rows, "concentration", &parameters.at("concentrations"));
    }

    Json get_spectra_headers(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = base::utils::input_rows(access, parameters, "spectraHeadersTable",
                                     {"analysis", "index", "scan", "array_length", "level", "mode", "polarity", "configuration", "lowmz",
                                      "highmz", "bpmz", "bpint", "tic", "rt", "mobility", "window_mz", "window_mzlow", "window_mzhigh",
                                      "precursor_mz", "precursor_intensity", "precursor_charge", "activation_ce"},
                                     "analysis");
        const auto wanted = analysis_names(parameters);
        Json result = Json::array();
        for (auto row : rows)
            if (selected(wanted, row.value("analysis", std::string{})))
                result.push_back(std::move(row));
        return result;
    }

    Json get_chromatograms_headers(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto rows = base::utils::input_rows(access, parameters, "chromatogramsHeadersTable",
                                     {"analysis", "index", "chromatogram_id", "array_length", "polarity", "precursor_mz", "activation_ce", "product_mz",
                                      "signal_type", "chromatogram_type", "detector", "channel", "units", "wavelength_nm", "interval_ms", "start_time", "end_time", "intensity_multiplier"},
                                     "analysis");
        const auto wanted = analysis_names(parameters);
        Json result = Json::array();
        for (auto row : rows)
            if (selected(wanted, row.value("analysis", std::string{})))
                result.push_back(std::move(row));
        return result;
    }

    Json get_spectra_tic(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto headers = get_spectra_headers(access, parameters);
        const auto analyses = selected_analysis_rows(access, parameters);
        std::map<std::string, std::string> replicates;
        for (const auto &row : analyses)
            replicates[row.value("analysis", std::string{})] = row.value("replicate", std::string{});
        const auto levels = parameters.value("levels", Json::array());
        Json result = Json::array();
        for (const auto &header : headers)
        {
            const auto level = std::stoi(header.value("level", "0"));
            const auto rt = std::stod(header.value("rt", "0"));
            if ((!levels.empty() && std::find(levels.begin(), levels.end(), level) == levels.end()) ||
                (parameters.contains("rt_min") && rt < parameters.at("rt_min").get<double>()) ||
                (parameters.contains("rt_max") && rt > parameters.at("rt_max").get<double>()))
                continue;
            const auto analysis = header.value("analysis", std::string{});
            result.push_back({{"analysis", analysis}, {"replicate", replicates[analysis]}, {"polarity", header.value("polarity", "0")}, {"level", level}, {"rt", rt}, {"mobility", header.value("mobility", "0")}, {"tic", header.value("tic", "0")}, {"bpmz", header.value("bpmz", "0")}, {"bpint", header.value("bpint", "0")}});
        }
        return result;
    }

    Json get_raw_spectra(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto analyses = selected_analysis_rows(access, parameters);
        const auto targets = base::utils::normalize_targets(parameters);
        const auto indices = parameters.value("indices", Json::array());
        const bool indexed = !indices.empty();
        Json result = Json::array();
        for (const auto &row : analyses)
        {
            const auto analysis = row.value("analysis", std::string{});
            if (!selected(analysis_names(parameters), analysis))
                continue;
            ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
            file.select_analysis(std::stoi(row.value("analysis_index", "0")));
            std::vector<int> selected_indices;
            for (const auto &value : indices)
                selected_indices.push_back(value.get<int>());
            const auto headers = indexed ? file.get_spectra_headers(selected_indices) : file.get_spectra_headers();
            const auto spectra = indexed ? file.get_spectra(selected_indices) : file.get_spectra();
            const auto requested_levels = parameters.value("levels", Json::array());
            for (std::size_t i = 0; i < spectra.size() && i < headers.index.size(); ++i)
                for (std::size_t j = 0; j < spectra[i][0].size() && j < spectra[i][1].size(); ++j)
                {
                    const auto mz = spectra[i][0][j];
                    const auto level = headers.level[i];
                    const double minimum_intensity = level == 1
                                                         ? parameters.value("min_intensity_ms1", 0.0)
                                                         : parameters.value("min_intensity_ms2", 0.0);
                    if ((!requested_levels.empty() && std::find(requested_levels.begin(), requested_levels.end(), level) == requested_levels.end()) ||
                        spectra[i][1][j] < minimum_intensity)
                        continue;
                    Json point = {{"analysis", analysis}, {"replicate", row.value("replicate", std::string{})}, {"id", analysis + ":" + std::to_string(headers.index[i])}, {"polarity", headers.polarity[i]}, {"level", headers.level[i]}, {"precursor_mz", headers.precursor_mz[i]}, {"activation_ce", headers.activation_ce[i]}, {"rt", headers.rt[i]}, {"mobility", headers.mobility[i]}, {"mz", mz}, {"intensity", spectra[i][1][j]}};
                    if (indexed)
                        result.push_back(std::move(point));
                    else
                        for (const auto &target : targets)
                            if (base::utils::target_matches(target, analysis, headers.polarity[i], headers.level[i], headers.rt[i], mz))
                            {
                                point["target_id"] = target.id;
                                result.push_back(point);
                            }
                }
        }
        return result;
    }

    Json get_raw_spectra_eic(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({1});
        return base::utils::summarize_eic(get_raw_spectra(access, copy));
    }

    Json get_raw_spectra_ms1(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({1});
        return base::utils::merge_ms_rows(get_raw_spectra(access, copy), copy.value("mz_clust", 0.003), copy.value("presence", 0.8));
    }

    Json get_raw_spectra_ms2(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({2});
        return base::utils::merge_ms_rows(get_raw_spectra(access, copy), copy.value("mz_clust", 0.005), copy.value("presence", 0.0));
    }

    Json get_raw_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto analyses = selected_analysis_rows(access, parameters);
        const auto indices = parameters.value("indices", Json::array());
        Json result = Json::array();
        for (const auto &row : analyses)
        {
            const auto analysis = row.value("analysis", std::string{});
            if (!selected(analysis_names(parameters), analysis))
                continue;
            ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
            file.select_analysis(std::stoi(row.value("analysis_index", "0")));
            std::vector<int> selected_indices;
            for (const auto &value : indices)
                selected_indices.push_back(value.get<int>());
            const auto headers = file.get_chromatograms_headers(selected_indices);
            const auto arrays = file.get_chromatograms(selected_indices);
            for (std::size_t i = 0; i < arrays.size() && i < headers.chromatogram_id.size(); ++i)
                for (std::size_t j = 0; j < arrays[i][0].size() && j < arrays[i][1].size(); ++j)
                    result.push_back({{"analysis", analysis}, {"replicate", row.value("replicate", std::string{})}, {"index", headers.index[i]}, {"chromatogram_id", headers.chromatogram_id[i]}, {"polarity", headers.polarity[i]}, {"precursor_mz", headers.precursor_mz[i]}, {"activation_ce", headers.activation_ce[i]}, {"product_mz", headers.product_mz[i]}, {"signal_type", headers.signal_type[i]}, {"chromatogram_type", headers.chromatogram_type[i]}, {"detector", headers.detector[i]}, {"channel", headers.channel[i]}, {"units", headers.units[i]}, {"wavelength_nm", headers.wavelength_nm[i]}, {"rt", arrays[i][0][j]}, {"raw_intensity", arrays[i][1][j]}, {"baseline", 0.0}, {"intensity", arrays[i][1][j]}});
        }
        return result;
    }

} // namespace streamfind::mass_spec::base
