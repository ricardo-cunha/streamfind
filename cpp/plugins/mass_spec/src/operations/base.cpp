#include "operations/base.hpp"
#include "readers/reader.hpp"
#include "streamfind/core/vendors/openbabel.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"
#include "utils/base.hpp"
#include "utils/chromatograms.hpp"
#include "utils/target_csv.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
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

        struct SpectrumPoint
        {
            std::string analysis, replicate, name;
            std::int64_t polarity = 0, level = 0;
            double precursor_mz = 0.0, precursor_mz_low = 0.0, precursor_mz_high = 0.0;
            double collision_energy = 0.0, rt = 0.0, mobility = 0.0, mz = 0.0, intensity = 0.0;
        };

        const std::vector<std::string> spectrum_point_columns = {
            "analysis", "replicate", "name", "polarity", "level", "pre_mz", "pre_mzlow", "pre_mzhigh",
            "pre_ce", "rt", "mobility", "mz", "intensity"};

        sdk::TableBatchBuilder spectrum_point_builder()
        {
            return sdk::TableBatchBuilder({
                {"analysis", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"replicate", STREAMFIND_PLUGIN_COLUMN_UTF8},
                {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64},
                {"level", STREAMFIND_PLUGIN_COLUMN_INT64}, {"pre_mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
                {"pre_mzlow", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"pre_mzhigh", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
                {"pre_ce", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
                {"mobility", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
                {"intensity", STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
        }

        void append_spectrum_point(sdk::TableBatchBuilder &builder, const SpectrumPoint &point)
        {
            builder.append_row({point.analysis, point.replicate, point.name, point.polarity, point.level,
                point.precursor_mz, point.precursor_mz_low, point.precursor_mz_high, point.collision_energy,
                point.rt, point.mobility, point.mz, point.intensity});
        }

        void emit_spectrum_points(sdk::PluginProjectAccess &access, const char *port,
                                  const std::vector<SpectrumPoint> &points)
        {
            auto output = spectrum_point_builder();
            for (const auto &point : points) {
                append_spectrum_point(output, point);
                if (output.row_count() >= 2048) {
                    output.emit(access, port);
                    output.clear();
                }
            }
            if (output.row_count() > 0 || points.empty()) output.emit(access, port);
        }

        std::vector<SpectrumPoint> merge_spectrum_points(const std::vector<SpectrumPoint> &points,
                                                         double mz_cluster, double presence)
        {
            using GroupKey = std::tuple<std::string, std::string, int, int, double, double>;
            std::map<GroupKey, std::vector<SpectrumPoint>> groups;
            for (const auto &point : points)
                groups[{point.analysis, point.name, static_cast<int>(point.polarity),
                        static_cast<int>(point.level), point.rt, point.precursor_mz}].push_back(point);
            std::vector<SpectrumPoint> result;
            for (auto &[key, values] : groups) {
                std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) { return left.mz < right.mz; });
                std::set<double> all_rt;
                for (const auto &point : values) all_rt.insert(point.rt);
                for (std::size_t start = 0; start < values.size();) {
                    std::size_t end = start + 1;
                    while (end < values.size() && values[end].mz - values[end - 1].mz <= mz_cluster) ++end;
                    std::set<double> cluster_rt;
                    for (std::size_t index = start; index < end; ++index) cluster_rt.insert(values[index].rt);
                    if (presence > 0.0 && static_cast<double>(cluster_rt.size()) < presence * static_cast<double>(all_rt.size())) {
                        start = end;
                        continue;
                    }
                    auto merged = values[start];
                    double maximum_intensity = 0.0, weighted_mz = 0.0, rt_sum = 0.0;
                    for (std::size_t index = start; index < end; ++index) {
                        const auto &point = values[index];
                        maximum_intensity = std::max(maximum_intensity, point.intensity);
                        weighted_mz += point.mz * point.intensity;
                        rt_sum += point.rt;
                    }
                    merged.mz = weighted_mz == 0.0 ? merged.mz : weighted_mz / std::max(maximum_intensity, 1e-12);
                    merged.intensity = maximum_intensity;
                    merged.rt = rt_sum / static_cast<double>(end - start);
                    result.push_back(std::move(merged));
                    start = end;
                }
            }
            return result;
        }

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char character)
                           { return static_cast<char>(std::tolower(character)); });
            return value;
        }

        std::string trim(std::string value)
        {
            auto first = value.begin();
            while (first != value.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
            auto last = value.end();
            while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
            return std::string(first, last);
        }

        void harmonize_chromatogram_ids(::mass_spec::reader::MASS_SPEC_CHROMATOGRAMS_HEADERS &headers)
        {
            for (std::size_t index = 0; index < headers.chromatogram_id.size(); ++index)
            {
                if (headers.chromatogram_id[index].empty())
                    headers.chromatogram_id[index] = "chromatogram_" + std::to_string(index);
            }
        }

        void emit_typed_rows(sdk::PluginProjectAccess &access, const std::string &contract,
                             const std::vector<StringRow> &source,
                             const std::vector<std::string> &names,
                             const std::vector<std::string> &types)
        {
            if (names.size() != types.size())
                throw std::invalid_argument("reader output column names and types differ in length");
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(names.size());
            for (std::size_t index = 0; index < names.size(); ++index) {
                const auto type = types[index] == "integer" ? STREAMFIND_PLUGIN_COLUMN_INT64 :
                    types[index] == "real" ? STREAMFIND_PLUGIN_COLUMN_FLOAT64 :
                    types[index] == "boolean" ? STREAMFIND_PLUGIN_COLUMN_BOOL : STREAMFIND_PLUGIN_COLUMN_UTF8;
                schema.push_back({names[index], type});
            }
            sdk::TableBatchBuilder output(std::move(schema));
            for (const auto &source_row : source)
            {
                if (source_row.size() != names.size())
                    throw std::invalid_argument("reader output row does not match its declared schema");
                std::vector<sdk::TableBatchBuilder::Cell> row;
                row.reserve(names.size());
                for (std::size_t index = 0; index < names.size(); ++index)
                {
                    if (!source_row[index].has_value())
                    {
                        row.emplace_back(std::monostate{});
                        continue;
                    }
                    const auto &value = *source_row[index];
                    if (types[index] == "integer")
                        row.emplace_back(static_cast<std::int64_t>(std::stoll(value)));
                    else if (types[index] == "real")
                        row.emplace_back(std::stod(value));
                    else if (types[index] == "boolean")
                        row.emplace_back(value == "true");
                    else
                        row.emplace_back(value);
                }
                output.append_row(row);
            }
            output.emit(access, contract);
        }

    } // namespace

    int analysis_index(const Json &row)
    {
        const auto value = row.find("analysis_index");
        if (value == row.end() || value->is_null()) return 0;
        if (value->is_number_integer()) return value->get<int>();
        return std::stoi(value->get<std::string>());
    }

    int json_integer(const Json &row, const char *key)
    {
        const auto value = row.find(key);
        if (value == row.end() || value->is_null()) return 0;
        return value->is_number() ? value->get<int>() : std::stoi(value->get<std::string>());
    }

    double json_real(const Json &row, const char *key)
    {
        const auto value = row.find(key);
        if (value == row.end() || value->is_null()) return 0.0;
        return value->is_number() ? value->get<double>() : std::stod(value->get<std::string>());
    }

    Json json_value_or_zero(const Json &row, const char *key)
    {
        const auto value = row.find(key);
        return value == row.end() || value->is_null() ? Json(0) : *value;
    }

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
            "start_rt", "end_rt", "has_ion_mobility", "concentration"};
        const std::vector<std::string> analysis_types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real"};
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
                const std::string analysis = path.stem().string() + "_" + descriptor.name;
                analysis_rows.push_back({analysis, std::to_string(descriptor.analysis_index),
                                         std::to_string(descriptor.source_analysis_number),
                                         replicate, blank, path.filename().string(), path.string(), path.parent_path().string(),
                                         lower(path.extension().string()), summary.format, "MS", summary.time_stamp,
                                         std::to_string(summary.number_spectra), std::to_string(summary.number_chromatograms),
                                         std::to_string(summary.number_spectra_binary_arrays), std::to_string(summary.min_mz),
                                         std::to_string(summary.max_mz), std::to_string(summary.start_rt), std::to_string(summary.end_rt),
                                         summary.has_ion_mobility ? "true" : "false", std::nullopt});
                Json added_analysis = {{"analysis", analysis}, {"file_path", path.string()},
                                       {"analysis_index", descriptor.analysis_index},
                                       {"source_analysis_number", descriptor.source_analysis_number},
                                       {"replicate", replicate}, {"blank", blank}};
                const auto calibration_provenance = file.get_calibration_provenance();
                if (!calibration_provenance.empty())
                    added_analysis["calibration_provenance"] = calibration_provenance;
                added.push_back(std::move(added_analysis));

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
                                                 std::to_string(chromatograms.polarity[index]), std::to_string(chromatograms::utils::normalize_mz(chromatograms.precursor_mz[index])),
                                                 std::to_string(chromatograms.activation_ce[index]), std::to_string(chromatograms::utils::normalize_mz(chromatograms.product_mz[index])),
                                                 chromatograms.signal_type[index], chromatograms.chromatogram_type[index], chromatograms.detector[index],
                                                 chromatograms.channel[index], chromatograms.units[index], std::to_string(chromatograms.wavelength_nm[index]),
                                                 std::to_string(chromatograms.interval_ms[index]), std::to_string(chromatograms.start_time[index]),
                                                 std::to_string(chromatograms.end_time[index]), std::to_string(chromatograms.intensity_multiplier[index])});
            }
        }

        if (!analysis_rows.empty()) emit_typed_rows(access, "analysesTable", analysis_rows, analysis_columns, analysis_types);
        if (!spectra_rows.empty()) emit_typed_rows(access, "spectraHeadersTable", spectra_rows, spectra_columns, spectra_types);
        if (!chromatogram_rows.empty()) emit_typed_rows(access, "chromatogramsHeadersTable", chromatogram_rows, chromatogram_columns, chromatogram_types);
        return added;
    }

    Json read_csv_targets(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto path = parameters.value("targets_csv_path", std::string{});
        if (path.empty()) throw std::invalid_argument("targets_csv_path must be a non-empty CSV file path");
        const auto parsed = target_csv::read_targets_csv(path, false);
        streamfind::sdk::TableBatchBuilder output({
            {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"analysis", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"level", STREAMFIND_PLUGIN_COLUMN_INT64},
            {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"mass_min", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"mass_max", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"mzmin", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"mzmax", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"rtmin", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"rtmax", STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
        for (const auto &source : parsed)
        {
            const auto string_value = [&source](const char *key) -> streamfind::sdk::TableBatchBuilder::Cell {
                const auto value = source.find(key);
                if (value == source.end() || value->is_null()) return std::monostate{};
                if (value->is_string()) return value->get<std::string>();
                return value->dump();
            };
            const auto number_value = [&source](const char *key, const char *alternate,
                                                 bool integer) -> streamfind::sdk::TableBatchBuilder::Cell {
                auto value = source.find(key);
                if (value == source.end() && alternate != nullptr) value = source.find(alternate);
                if (value == source.end() || value->is_null()) return std::monostate{};
                if (integer) {
                    if (value->is_number_integer()) return value->get<std::int64_t>();
                    if (value->is_string()) return static_cast<std::int64_t>(std::stoll(value->get<std::string>()));
                    return std::monostate{};
                }
                if (value->is_number()) return value->get<double>();
                if (value->is_string()) return std::stod(value->get<std::string>());
                return std::monostate{};
            };
            output.append_row({string_value("name"), string_value("analysis"),
                number_value("polarity", nullptr, true), number_value("level", nullptr, true),
                number_value("mass", nullptr, false), number_value("mass_min", nullptr, false),
                number_value("mass_max", nullptr, false), number_value("mz", nullptr, false),
                number_value("mzmin", "mz_min", false), number_value("mzmax", "mz_max", false),
                number_value("rt", nullptr, false), number_value("rtmin", "rt_min", false),
                number_value("rtmax", "rt_max", false)});
        }
        output.emit(access, "targetsTable");
        return {{"rows", parsed.size()}, {"path", path}};
    }

    Json read_mol_suspect_target(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto path = parameters.value("mol_file_path", std::string{});
        if (path.empty()) throw std::invalid_argument("mol_file_path must be a non-empty MOL file path");

        std::ifstream input(path);
        if (!input) throw std::invalid_argument("Cannot open MOL file: " + path);
        std::string name = trim(parameters.value("compound_name", std::string{}));
        std::string mol_title;
        std::getline(input, mol_title);
        if (name.empty())
        {
            name = trim(mol_title);
        }
        if (name.empty()) name = "Compound";

        const auto structure = ::streamfind::core::vendors::openbabel::normalize_structure_from_mol_file(path);
        if (!structure.ok)
            throw std::invalid_argument("Could not normalize MOL structure: " + structure.error);

        streamfind::sdk::TableBatchBuilder output({
            {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"formula", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"SMILES", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"InChI", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"InChIKey", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"xLogP", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_mz_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_intensity_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_mz_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_intensity_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
        output.append_row({name, structure.exact_mass, std::monostate{}, std::monostate{}, std::monostate{},
            structure.formula, structure.canonical_smiles, structure.inchi, structure.inchikey,
            structure.has_xlogp ? streamfind::sdk::TableBatchBuilder::Cell{structure.xlogp}
                                : streamfind::sdk::TableBatchBuilder::Cell{std::monostate{}},
            std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{}});
        output.emit(access, "suspectTargetsTable");
        return {{"rows", 1}, {"path", path}};
    }


    Json remove_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto &inputs = parameters.at("_inputs");
        const auto &analyses_input = inputs.at("analysesTable");
        const auto result_ref = analyses_input.at("result_ref").get<std::string>();
        const std::vector<std::string> columns = {
            "analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
            "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
            "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
            "start_rt", "end_rt", "has_ion_mobility", "concentration"};
        const std::vector<std::string> types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real"};
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

        std::vector<streamfind::sdk::TableBatchBuilder::Column> output_schema;
        output_schema.reserve(columns.size());
        for (std::size_t index = 0; index < columns.size(); ++index)
        {
            const auto &type = types[index];
            if (type == "integer") output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_INT64});
            else if (type == "real") output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_FLOAT64});
            else if (type == "boolean") output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_BOOL});
            else if (type == "timestamp") output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_TIMESTAMP});
            else if (type == "decimal") output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_DECIMAL});
            else output_schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8});
        }
        streamfind::sdk::TableBatchBuilder output(std::move(output_schema));
        Json removed = Json::array();
        access.read_typed_batches(result_ref, columns,
            [&](const streamfind::sdk::PluginProjectAccess::BatchView &batch) {
                const auto analysis_column = batch.column_index("analysis");
                for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                {
                    const auto analysis = batch.is_null(analysis_column, row)
                        ? std::string{} : std::string(batch.text(analysis_column, row));
                    const bool matches_literal = std::find(literal_patterns.begin(), literal_patterns.end(), analysis) != literal_patterns.end();
                    const bool matches_regex = std::any_of(patterns.begin(), patterns.end(), [&](const auto &pattern) {
                        return std::regex_search(analysis, pattern);
                    });
                    if (matches_literal || matches_regex) removed.push_back(analysis);
                    else output.append_row(batch, row);
                }
            });
        output.emit(access, "analysesTable");
        return removed;
    }

    namespace
    {


        const std::vector<std::string> analysis_columns = {
            "analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
            "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
            "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
            "start_rt", "end_rt", "has_ion_mobility", "concentration"};
        const std::vector<std::string> analysis_types = {
            "string", "integer", "integer", "string", "string", "string", "string", "string", "string",
            "string", "string", "string", "integer", "integer", "integer", "real", "real", "real", "real", "boolean", "real"};

        bool selected(const Json &wanted, const std::string &name, int index = 0);

        Json selected_analysis_rows(sdk::PluginProjectAccess &access, const Json &parameters,
                                    const std::vector<std::string> &columns = analysis_columns,
                                    bool filter_indices = false);

        Json analysis_value_rows(sdk::PluginProjectAccess &access, const Json &parameters, const char *column,
                                 bool numeric)
        {
            const auto rows = selected_analysis_rows(access, parameters, {column}, true);
            Json result = Json::array();
            for (const auto &row : rows)
            {
                const auto value = row.value(column, std::string{});
                result.push_back(numeric && !value.empty() ? Json(std::stod(value)) : Json(value));
            }
            return result;
        }

        Json selected_analysis_rows(sdk::PluginProjectAccess &access, const Json &parameters,
                                    const std::vector<std::string> &columns, bool filter_indices)
        {
            const auto rows = base::utils::read_control_rows(access, parameters, "analysesTable", columns, "analysis");
            const auto wanted_names = parameters.value("analysis_names", Json::array());
            const auto wanted_indices = filter_indices ? parameters.value("indices", Json::array()) : Json::array();

            // A target row may carry its own analysis selector.  This is the
            // authoritative selector for target-driven operations such as EIC
            // extraction; an empty global analysis_names parameter must not
            // broaden those rows back to every analysis.
            std::set<std::string> target_analysis_names;
            std::set<int> target_analysis_indices;
            bool has_target_analysis_constraint = false;
            bool has_unrestricted_target = false;
            const auto targets = parameters.value("targets", Json::array());
            if (targets.is_array() && !targets.empty())
            {
                for (const auto &target : targets)
                {
                    if (!target.is_object() || !target.contains("analysis") || target.at("analysis").is_null())
                    {
                        has_unrestricted_target = true;
                        continue;
                    }
                    const auto &analysis = target.at("analysis");
                    bool added_analysis = false;
                    const auto add_analysis = [&](const Json &value) {
                        if (value.is_string() && !value.get<std::string>().empty()) {
                            target_analysis_names.insert(value.get<std::string>());
                            added_analysis = true;
                        } else if (value.is_number_integer()) {
                            target_analysis_indices.insert(value.get<int>());
                            added_analysis = true;
                        }
                    };
                    if (analysis.is_array())
                        for (const auto &value : analysis) add_analysis(value);
                    else
                        add_analysis(analysis);
                    if (added_analysis) has_target_analysis_constraint = true;
                    else has_unrestricted_target = true;
                }
            }
            Json selected_rows = Json::array();
            for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
            {
                const auto name = rows[row_index].value("analysis", std::string{});
                if (!selected(wanted_names, name, static_cast<int>(row_index)))
                    continue;
                const auto data_index = rows[row_index].contains("index")
                    ? json_integer(rows[row_index], "index")
                    : static_cast<int>(row_index);
                if (!wanted_indices.empty() &&
                    std::find(wanted_indices.begin(), wanted_indices.end(), data_index) == wanted_indices.end())
                    continue;
                if (has_target_analysis_constraint && !has_unrestricted_target &&
                    !target_analysis_names.contains(name) && !target_analysis_indices.contains(data_index))
                    continue;
                selected_rows.push_back(rows[row_index]);
            }
            return selected_rows;
        }

        Json set_analysis_values(sdk::PluginProjectAccess &access, const Json &parameters,
                                 const char *column_name, const Json &replacement_values)
        {
            if (!replacement_values.is_array())
                throw std::invalid_argument(std::string(column_name) + " values must be an array");
            const auto wanted_names = parameters.value("analysis_names", Json::array());
            const auto wanted_indices = parameters.value("indices", Json::array());
            auto make_builder = [] {
                std::vector<sdk::TableBatchBuilder::Column> schema;
                schema.reserve(analysis_columns.size());
                for (std::size_t index = 0; index < analysis_columns.size(); ++index) {
                    auto type = STREAMFIND_PLUGIN_COLUMN_UTF8;
                    if (analysis_types[index] == "integer") type = STREAMFIND_PLUGIN_COLUMN_INT64;
                    else if (analysis_types[index] == "real") type = STREAMFIND_PLUGIN_COLUMN_FLOAT64;
                    else if (analysis_types[index] == "boolean") type = STREAMFIND_PLUGIN_COLUMN_BOOL;
                    schema.push_back({analysis_columns[index], type});
                }
                return sdk::TableBatchBuilder(std::move(schema));
            };
            auto output = make_builder();
            std::size_t selected_count = 0;
            std::size_t source_index = 0;
            access.read_typed_batches(base::utils::input_table(parameters, "analysesTable"),
                analysis_columns, [&](const sdk::PluginProjectAccess::BatchView &batch) {
                    const auto analysis_column = batch.column_index("analysis");
                    for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++source_index) {
                        const auto name = batch.is_null(analysis_column, row)
                            ? std::string{} : std::string(batch.text(analysis_column, row));
                        if (!selected(wanted_names, name, static_cast<int>(source_index))) continue;
                        if (!wanted_indices.empty() &&
                            std::find(wanted_indices.begin(), wanted_indices.end(), static_cast<int>(source_index)) == wanted_indices.end())
                            continue;
                        if (selected_count >= replacement_values.size())
                            throw std::invalid_argument(std::string(column_name) + " length must match selected analyses");
                        const auto &replacement = replacement_values.at(selected_count++);
                        if (!replacement.is_string())
                            throw std::invalid_argument(std::string(column_name) + " values must be strings");
                        output.append_row(batch, row, {{column_name, replacement.get<std::string>()}});
                    }
                });
            if (selected_count != replacement_values.size())
                throw std::invalid_argument(std::string(column_name) + " length must match selected analyses");
            output.emit(access, "analysesTable");
            return Json{{"updated", selected_count}};
        }

        Json analysis_names(const Json &parameters)
        {
            return parameters.value("analysis_names", Json::array());
        }

        std::map<std::string, int> analysis_row_indices(sdk::PluginProjectAccess &access, const Json &parameters)
        {
            const auto rows = base::utils::read_control_rows(access, parameters, "analysesTable", {"analysis"}, "analysis");
            std::map<std::string, int> result;
            for (std::size_t index = 0; index < rows.size(); ++index)
                result[rows[index].value("analysis", std::string{})] = static_cast<int>(index);
            return result;
        }

        bool selected(const Json &wanted, const std::string &name, int index)
                {
                    if (wanted.empty())
                return true;
            for (const auto &value : wanted)
            {
                if (value.is_string() && value.get<std::string>() == name)
                    return true;
                if (value.is_number_integer() && value.get<int>() == index)
                    return true;
            }
            return false;
        }

    } // namespace

    Json get_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return selected_analysis_rows(access, parameters,
                          {"analysis", "analysis_index", "source_analysis_number", "replicate", "blank",
                           "file_path", "format", "number_spectra", "number_chromatograms"}, true);
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
        return set_analysis_values(access, parameters, "replicate", parameters.at("replicate_names"));
    }

    Json set_blank_names(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return set_analysis_values(access, parameters, "blank", parameters.at("blank_names"));
    }

    Json set_concentrations(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return set_analysis_values(access, parameters, "concentration", parameters.at("concentrations"));
    }

    Json get_spectra_headers(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const std::vector<std::string> columns = {
            "analysis", "index", "scan", "array_length", "level", "mode", "polarity", "configuration", "lowmz",
            "highmz", "bpmz", "bpint", "tic", "rt", "mobility", "window_mz", "window_mzlow", "window_mzhigh",
            "precursor_mz", "precursor_intensity", "precursor_charge", "activation_ce"};
        const auto wanted = analysis_names(parameters);
        const auto analysis_indices = analysis_row_indices(access, parameters);
        const auto indices = parameters.value("indices", Json::array());
        const auto table = base::utils::input_table(parameters, "spectraHeadersTable");
        std::size_t emitted = 0;
        access.read_typed_batches(table, columns, [&](const auto &batch) {
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(columns.size());
            for (std::uint32_t column = 0; column < batch.column_count(); ++column)
                schema.push_back({columns.at(column), batch.type(column), batch.is_list(column),
                    batch.is_list(column) ? batch.list_child_type(column) : STREAMFIND_PLUGIN_COLUMN_UTF8});
            sdk::TableBatchBuilder output(std::move(schema));
            const auto analysis_col = batch.column_index("analysis");
            const auto index_col = batch.column_index("index");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                const auto analysis = batch.is_null(analysis_col, row)
                    ? std::string{} : std::string(batch.text(analysis_col, row));
                const auto analysis_it = analysis_indices.find(analysis);
                if (analysis_it == analysis_indices.end() || !selected(wanted, analysis, analysis_it->second)) continue;
                const auto index = batch.is_null(index_col, row) ? 0 : static_cast<int>(batch.integer(index_col, row));
                if (!indices.empty() && std::find(indices.begin(), indices.end(), index) == indices.end()) continue;
                output.append_row(batch, row);
                if (output.row_count() >= 1024) {
                    emitted += static_cast<std::size_t>(output.row_count());
                    output.emit(access, "spectraHeadersTable");
                    output.clear();
                }
            }
            if (output.row_count() != 0) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, "spectraHeadersTable");
            }
        });
        return {{"status", "finished"}, {"rows", emitted}};
    }

    Json get_chromatograms_headers(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const std::vector<std::string> columns = {
            "analysis", "index", "chromatogram_id", "array_length", "polarity", "precursor_mz", "activation_ce", "product_mz",
            "signal_type", "chromatogram_type", "detector", "channel", "units", "wavelength_nm", "interval_ms", "start_time", "end_time", "intensity_multiplier"};
        const auto wanted = analysis_names(parameters);
        const auto analysis_indices = analysis_row_indices(access, parameters);
        const auto indices = parameters.value("indices", Json::array());
        const auto table = base::utils::input_table(parameters, "chromatogramsHeadersTable");
        std::size_t emitted = 0;
        access.read_typed_batches(table, columns, [&](const auto &batch) {
            std::vector<sdk::TableBatchBuilder::Column> schema;
            schema.reserve(columns.size());
            for (std::uint32_t column = 0; column < batch.column_count(); ++column)
                schema.push_back({columns.at(column), batch.type(column), batch.is_list(column),
                    batch.is_list(column) ? batch.list_child_type(column) : STREAMFIND_PLUGIN_COLUMN_UTF8});
            sdk::TableBatchBuilder output(std::move(schema));
            const auto analysis_col = batch.column_index("analysis");
            const auto index_col = batch.column_index("index");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                const auto analysis = batch.is_null(analysis_col, row)
                    ? std::string{} : std::string(batch.text(analysis_col, row));
                const auto analysis_it = analysis_indices.find(analysis);
                if (analysis_it == analysis_indices.end() || !selected(wanted, analysis, analysis_it->second)) continue;
                const auto index = batch.is_null(index_col, row) ? 0 : static_cast<int>(batch.integer(index_col, row));
                if (!indices.empty() && std::find(indices.begin(), indices.end(), index) == indices.end()) continue;
                output.append_row(batch, row);
                if (output.row_count() >= 1024) {
                    emitted += static_cast<std::size_t>(output.row_count());
                    output.emit(access, "chromatogramsHeadersTable");
                    output.clear();
                }
            }
            if (output.row_count() != 0) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, "chromatogramsHeadersTable");
            }
        });
        return {{"status", "finished"}, {"rows", emitted}};
    }

    namespace detail {

    struct SpectraTicRow {
        std::string analysis;
        std::string replicate;
        std::optional<std::int64_t> polarity;
        std::optional<std::int64_t> level;
        std::optional<double> rt;
        std::optional<double> mobility;
        std::optional<double> tic;
        std::optional<double> bpmz;
        std::optional<double> bpint;
    };

    template <typename Callback>
    void visit_spectra_tic_rows(sdk::PluginProjectAccess &access,
                                const Json &parameters,
                                Callback &&callback)
    {
        const auto analyses = selected_analysis_rows(access, parameters);
        std::map<std::string, std::string> replicates;
        for (const auto &row : analyses)
            replicates[row.value("analysis", std::string{})] = row.value("replicate", std::string{});
        const auto levels = parameters.value("levels", Json::array());
        const auto wanted = analysis_names(parameters);
        const auto analysis_indices = analysis_row_indices(access, parameters);
        const auto indices = parameters.value("indices", Json::array());
        const std::vector<std::string> columns = {
            "analysis", "index", "level", "polarity", "rt", "mobility", "tic", "bpmz", "bpint"};
        const auto table = base::utils::input_table(parameters, "spectraHeadersTable");
        access.read_typed_batches(table, columns, [&](const auto &batch) {
            const auto analysis_col = batch.column_index("analysis");
            const auto index_col = batch.column_index("index");
            const auto level_col = batch.column_index("level");
            const auto polarity_col = batch.column_index("polarity");
            const auto rt_col = batch.column_index("rt");
            const auto mobility_col = batch.column_index("mobility");
            const auto tic_col = batch.column_index("tic");
            const auto bpmz_col = batch.column_index("bpmz");
            const auto bpint_col = batch.column_index("bpint");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                const auto analysis = batch.is_null(analysis_col, row)
                    ? std::string{} : std::string(batch.text(analysis_col, row));
                const auto analysis_it = analysis_indices.find(analysis);
                if (analysis_it == analysis_indices.end() || !selected(wanted, analysis, analysis_it->second)) continue;
                if (!indices.empty() && !batch.is_null(index_col, row) &&
                    std::find(indices.begin(), indices.end(), batch.integer(index_col, row)) == indices.end()) continue;
                const auto level = batch.is_null(level_col, row) ? 0 : static_cast<int>(batch.integer(level_col, row));
                const auto rt = batch.is_null(rt_col, row) ? 0.0 : batch.real(rt_col, row);
                const auto rt_min = parameters.value("rt_min", 0.0);
                const auto rt_max = parameters.value("rt_max", 0.0);
                if ((!levels.empty() && std::find(levels.begin(), levels.end(), level) == levels.end()) ||
                    (rt_min > 0.0 && rt < rt_min) ||
                    (rt_max > 0.0 && rt > rt_max)) continue;
                const auto optional_integer = [&](std::uint32_t column) -> std::optional<std::int64_t> {
                    if (batch.is_null(column, row)) return std::nullopt;
                    return batch.integer(column, row);
                };
                callback(SpectraTicRow{
                    analysis, replicates[analysis], optional_integer(polarity_col),
                    optional_integer(level_col),
                    batch.is_null(rt_col, row) ? std::nullopt : std::optional<double>(rt),
                    batch.is_null(mobility_col, row) ? std::nullopt : std::optional<double>(batch.real(mobility_col, row)),
                    batch.is_null(tic_col, row) ? std::nullopt : std::optional<double>(batch.real(tic_col, row)),
                    batch.is_null(bpmz_col, row) ? std::nullopt : std::optional<double>(batch.real(bpmz_col, row)),
                    batch.is_null(bpint_col, row) ? std::nullopt : std::optional<double>(batch.real(bpint_col, row))});
            }
        });
    }

    } // namespace detail

    Json get_spectra_tic(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        sdk::TableBatchBuilder output({
            {"analysis", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"replicate", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"level", STREAMFIND_PLUGIN_COLUMN_INT64},
            {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"mobility", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"tic", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"bpmz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"bpint", STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
        std::size_t emitted = 0;
        const auto integer_cell = [](const std::optional<std::int64_t> &value) -> sdk::TableBatchBuilder::Cell {
            return value ? sdk::TableBatchBuilder::Cell(*value) : sdk::TableBatchBuilder::Cell(std::monostate{});
        };
        const auto real_cell = [](const std::optional<double> &value) -> sdk::TableBatchBuilder::Cell {
            return value ? sdk::TableBatchBuilder::Cell(*value) : sdk::TableBatchBuilder::Cell(std::monostate{});
        };
        detail::visit_spectra_tic_rows(access, parameters, [&](const detail::SpectraTicRow &row) {
            output.append_row({row.analysis, row.replicate, integer_cell(row.polarity), integer_cell(row.level),
                real_cell(row.rt), real_cell(row.mobility), real_cell(row.tic), real_cell(row.bpmz),
                real_cell(row.bpint)});
            if (output.row_count() >= 1024) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, "ticTable");
                output.clear();
            }
        });
        if (output.row_count() != 0) {
            emitted += static_cast<std::size_t>(output.row_count());
            output.emit(access, "ticTable");
        }
        return {{"status", "finished"}, {"rows", emitted}};
    }

    static std::vector<SpectrumPoint> extract_raw_spectra(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto &effective_parameters = parameters;
        const auto analyses = selected_analysis_rows(access, effective_parameters);
        const auto targets = base::utils::normalize_targets(access, effective_parameters);
        const auto indices = parameters.value("indices", Json::array());
        const bool indexed = !indices.empty();
        const bool eic_logging = parameters.value("_eic_logging", false);
        std::vector<SpectrumPoint> result;
        for (std::size_t analysis_number = 0; analysis_number < analyses.size(); ++analysis_number)
        {
            const auto &row = analyses[analysis_number];
            const auto analysis = row.value("analysis", std::string{});
            const auto data_index = row.contains("index")
                ? row.at("index").get<int>()
                : static_cast<int>(analysis_number);
            std::vector<const base::utils::TargetRange *> active_targets;
            active_targets.reserve(targets.size());
            for (const auto &target : targets)
            {
                const bool name_selected = target.analyses.empty() ||
                    std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end();
                const bool index_selected = target.analysis_indices.empty() ||
                    std::find(target.analysis_indices.begin(), target.analysis_indices.end(), data_index) != target.analysis_indices.end();
                if (name_selected && index_selected)
                    active_targets.push_back(&target);
            }
            if (!indexed && active_targets.empty())
                continue;
            if (eic_logging)
                access.report_progress(
                    analyses.empty() ? 0.0 : static_cast<double>(analysis_number) / static_cast<double>(analyses.size()),
                    "EIC: reading analysis " + std::to_string(analysis_number + 1) + "/" +
                        std::to_string(analyses.size()) + " (" + analysis + ").");

            ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
            file.select_analysis(analysis_index(row));
            std::vector<int> selected_indices;
            for (const auto &value : indices)
                selected_indices.push_back(value.get<int>());
            const auto requested_levels = parameters.value("levels", Json::array());
            if (eic_logging && !indexed)
            {
                // The source file is authoritative for spectrum headers and
                // is already open for decoding.  Do not rescan the published
                // spectraHeadersTable through the data proxy for every
                // analysis; that turns a targeted EIC request into a full
                // result-store scan before any spectra are decoded.
                const auto source_headers = file.get_spectra_headers();
                for (std::size_t header_index = 0; header_index < source_headers.index.size(); ++header_index)
                {
                    const auto level = source_headers.level[header_index];
                    if (!requested_levels.empty() &&
                        std::find(requested_levels.begin(), requested_levels.end(), level) == requested_levels.end())
                        continue;
                    bool possible = false;
                    for (const auto *target : active_targets)
                    {
                        const auto low_mz = source_headers.lowmz[header_index];
                        const auto high_mz = source_headers.highmz[header_index];
                        // TOF readers may not know the spectrum m/z bounds
                        // until the compressed payload is decoded. Treat the
                        // placeholder range as unknown so targeted EIC does
                        // not discard every scan before decoding.
                        const bool unknown_mz_range = !std::isfinite(low_mz) || !std::isfinite(high_mz) || high_mz <= low_mz;
                        if (base::utils::target_may_match_spectrum(*target, analysis, source_headers.polarity[header_index], level,
                                                                    source_headers.rt[header_index],
                                                                    unknown_mz_range ? -std::numeric_limits<float>::infinity() : low_mz,
                                                                    unknown_mz_range ? std::numeric_limits<float>::infinity() : high_mz))
                        {
                            possible = true;
                            break;
                        }
                    }
                    if (possible)
                        selected_indices.push_back(source_headers.index[header_index]);
                }
            }
            if (eic_logging && selected_indices.empty())
                continue;
            const auto headers = (indexed || eic_logging)
                ? file.get_spectra_headers(selected_indices)
                : file.get_spectra_headers();

            std::unordered_map<int, std::size_t> header_positions;
            header_positions.reserve(headers.index.size());
            for (std::size_t i = 0; i < headers.index.size(); ++i)
                header_positions.emplace(headers.index[i], i);
            std::unordered_map<int, bool> candidate_lookup;
            candidate_lookup.reserve(selected_indices.size());
            for (const auto index : selected_indices)
                candidate_lookup.emplace(index, true);
            const auto &decode_indices = indexed ? selected_indices : headers.index;
            constexpr std::size_t decode_batch_size = 256;
            int last_logged_percent = -1;
            for (std::size_t batch_start = 0; batch_start < decode_indices.size(); batch_start += decode_batch_size)
            {
                const auto batch_end = std::min(batch_start + decode_batch_size, decode_indices.size());
                const std::vector<int> batch_indices(
                    decode_indices.begin() + static_cast<std::ptrdiff_t>(batch_start),
                    decode_indices.begin() + static_cast<std::ptrdiff_t>(batch_end));
                const auto spectra = file.get_spectra(batch_indices);
                for (std::size_t batch_offset = 0; batch_offset < spectra.size(); ++batch_offset)
                {
                    const auto position = header_positions.find(batch_indices[batch_offset]);
                    if (position == header_positions.end() || spectra[batch_offset].size() < 2)
                        continue;
                    const auto source = position->second;
                    if (!indexed && candidate_lookup.find(batch_indices[batch_offset]) == candidate_lookup.end())
                        continue;
                    for (std::size_t j = 0; j < spectra[batch_offset][0].size() && j < spectra[batch_offset][1].size(); ++j)
                    {
                        const auto mz = spectra[batch_offset][0][j];
                        const auto level = headers.level[source];
                        const double minimum_intensity = level == 1
                                                             ? parameters.value("min_intensity_ms1", 0.0)
                                                             : parameters.value("min_intensity_ms2", 0.0);
                        if ((!requested_levels.empty() && std::find(requested_levels.begin(), requested_levels.end(), level) == requested_levels.end()) ||
                            spectra[batch_offset][1][j] < minimum_intensity)
                            continue;
                        SpectrumPoint point{analysis, row.value("replicate", std::string{}), {},
                            headers.polarity[source], headers.level[source], headers.precursor_mz[source],
                            headers.window_mzlow[source], headers.window_mzhigh[source], headers.activation_ce[source],
                            headers.rt[source], headers.mobility[source], mz, spectra[batch_offset][1][j]};
                        if (indexed)
                            result.push_back(std::move(point));
                        else
                            for (const auto *target : active_targets)
                                if (base::utils::target_matches(*target, analysis, headers.polarity[source], headers.level[source], headers.rt[source], mz))
                                {
                                    // Target rows may arrive as a normalized
                                    // table value from the service.  The
                                    // matched target is therefore the source
                                    // of truth for the trace name, regardless
                                    // of the original parameter encoding.
                                    auto match = point;
                                    match.name = target->id;
                                    result.push_back(std::move(match));
                                }
                    }
                }
                if (eic_logging && !decode_indices.empty())
                {
                    const auto percent = static_cast<int>((75.0 * static_cast<double>(batch_end)) /
                                                          static_cast<double>(decode_indices.size()));
                    if (percent != last_logged_percent)
                    {
                        last_logged_percent = percent;
                        access.report_progress(percent / 100.0,
                            "EIC: " + analysis + " decoded " + std::to_string(batch_end) + "/" +
                            std::to_string(decode_indices.size()) + " spectra.");
                    }
                }
            }
        }
        if (!parameters.value("_suppress_raw_table", false))
            emit_spectrum_points(access, "spectraTable", result);
        return result;
    }

    Json get_raw_spectra(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto points = extract_raw_spectra(access, parameters);
        return Json{{"status", "finished"}, {"spectra_points", points.size()}};
    }

    Json get_raw_spectra_eic(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({1});
        copy["_suppress_raw_table"] = true;
        copy["_eic_logging"] = true;
        const auto analyses = selected_analysis_rows(access, parameters);
        const auto targets = base::utils::normalize_targets(access, parameters);
        access.report_progress(0.0, "EIC: preparing " + std::to_string(analyses.size()) +
            " analyses and " + std::to_string(targets.size()) + " targets.");
        auto extracted = extract_raw_spectra(access, copy);
        using EicKey = std::tuple<std::string, std::int64_t, std::string, double>;
        std::map<EicKey, SpectrumPoint> summarized;
        for (const auto &point : extracted) {
            const EicKey key{point.analysis, point.polarity, point.name, point.rt};
            auto [it, inserted] = summarized.emplace(key, point);
            if (!inserted) it->second.intensity = std::max(it->second.intensity, point.intensity);
        }
        std::vector<SpectrumPoint> result;
        result.reserve(summarized.size());
        for (auto &[_, point] : summarized) result.push_back(std::move(point));
        emit_spectrum_points(access, "eicTable", result);
        access.report_progress(1.0, "EIC extraction complete: " + std::to_string(result.size()) + " summarized rows.");
        return Json{{"status", "finished"}, {"spectra_points", extracted.size()}, {"eic_points", result.size()}};
    }

    Json get_raw_spectra_ms1(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({1});
        copy["_suppress_raw_table"] = true;
        const auto points = extract_raw_spectra(access, copy);
        auto result = merge_spectrum_points(points, copy.value("mz_clust", 0.003), copy.value("presence", 0.8));
        emit_spectrum_points(access, "ms1Table", result);
        return Json{{"status", "finished"}, {"spectra_points", points.size()}, {"merged_peaks", result.size()}};
    }

    Json get_raw_spectra_ms2(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        auto copy = parameters;
        copy["levels"] = Json::array({2});
        copy["_suppress_raw_table"] = true;
        const auto points = extract_raw_spectra(access, copy);
        auto result = merge_spectrum_points(points, copy.value("mz_clust", 0.005), copy.value("presence", 0.0));
        emit_spectrum_points(access, "ms2Table", result);
        return Json{{"status", "finished"}, {"spectra_points", points.size()}, {"merged_peaks", result.size()}};
    }

    Json get_raw_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto analyses = selected_analysis_rows(access, parameters);
        const auto indices = parameters.value("indices", Json::array());
        Json result = Json::array();
        for (const auto &row : analyses)
        {
            const auto analysis = row.value("analysis", std::string{});

            ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
            file.select_analysis(analysis_index(row));
            std::vector<int> selected_indices;
            for (const auto &value : indices)
                selected_indices.push_back(value.get<int>());
            const auto headers = file.get_chromatograms_headers(selected_indices);
            const auto arrays = file.get_chromatograms(selected_indices);
            for (std::size_t i = 0; i < arrays.size() && i < headers.chromatogram_id.size(); ++i)
                for (std::size_t j = 0; j < arrays[i][0].size() && j < arrays[i][1].size(); ++j)
                    result.push_back({{"analysis", analysis}, {"replicate", row.value("replicate", std::string{})}, {"index", headers.index[i]}, {"chromatogram_id", headers.chromatogram_id[i]}, {"polarity", headers.polarity[i]}, {"precursor_mz", chromatograms::utils::normalize_mz(headers.precursor_mz[i])}, {"activation_ce", headers.activation_ce[i]}, {"product_mz", chromatograms::utils::normalize_mz(headers.product_mz[i])}, {"signal_type", headers.signal_type[i]}, {"chromatogram_type", headers.chromatogram_type[i]}, {"detector", headers.detector[i]}, {"channel", headers.channel[i]}, {"units", headers.units[i]}, {"wavelength_nm", headers.wavelength_nm[i]}, {"rt", arrays[i][0][j]}, {"raw_intensity", arrays[i][1][j]}, {"baseline", 0.0}, {"intensity", arrays[i][1][j]}});
        }
        return result;
    }

} // namespace streamfind::mass_spec::base
