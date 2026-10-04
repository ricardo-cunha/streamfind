#include "utils/target_csv.hpp"

#include "streamfind/core/vendors/openbabel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace streamfind::mass_spec::target_csv
{
    namespace detail
    {
        std::string trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::vector<std::string> split_csv_line(const std::string &line)
        {
            std::vector<std::string> fields;
            std::string field;
            bool quoted = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (c == '"')
                {
                    if (quoted && i + 1 < line.size() && line[i + 1] == '"')
                    {
                        field.push_back('"');
                        ++i;
                    }
                    else quoted = !quoted;
                }
                else if (c == ',' && !quoted)
                {
                    fields.push_back(trim(field));
                    field.clear();
                }
                else field.push_back(c);
            }
            if (quoted) throw std::invalid_argument("CSV contains an unterminated quoted field");
            fields.push_back(trim(field));
            return fields;
        }

        bool present(const nlohmann::json &row, const char *key)
        {
            const auto it = row.find(key);
            return it != row.end() && !it->is_null() &&
                   (!(it->is_string()) || !trim(it->get<std::string>()).empty());
        }

        std::string text(const nlohmann::json &row, const char *key)
        {
            const auto it = row.find(key);
            if (it == row.end() || it->is_null()) return {};
            return it->is_string() ? trim(it->get<std::string>()) : it->dump();
        }

        double number(const nlohmann::json &row, const char *key)
        {
            const auto value = text(row, key);
            if (value.empty()) return 0.0;
            try { return std::stod(value); }
            catch (...) { throw std::invalid_argument(std::string("CSV column '") + key + "' must be numeric"); }
        }

        void rename_alias(nlohmann::json &row, const char *from, const char *to)
        {
            if (!present(row, to) && present(row, from)) row[to] = row[from];
        }

        nlohmann::json parse_fragment_pairs(const std::string &value)
        {
            nlohmann::json result = nlohmann::json::array();
            std::string pair;
            std::stringstream pairs(value);
            while (std::getline(pairs, pair, ';'))
            {
                std::stringstream values(trim(pair));
                double mz = 0.0, intensity = 0.0;
                if (values >> mz >> intensity) result.push_back({mz, intensity});
            }
            return result;
        }

        void set_fragment_columns(nlohmann::json &row, const char *source, const char *mz_column, const char *int_column)
        {
            const auto value = text(row, source);
            if (value.empty()) return;
            const auto pairs = parse_fragment_pairs(value);
            row[mz_column] = nlohmann::json::array();
            row[int_column] = nlohmann::json::array();
            for (const auto &pair : pairs)
            {
                row[mz_column].push_back(pair[0]);
                row[int_column].push_back(pair[1]);
            }
        }

        void normalize_structure(nlohmann::json &row, bool suspect_targets)
        {
            rename_alias(row, "smiles", "SMILES");
            rename_alias(row, "inchi", "InChI");
            rename_alias(row, "inchikey", "InChIKey");
            rename_alias(row, "xlogp", "xLogP");
            rename_alias(row, "mzmin", "mz_min");
            rename_alias(row, "mzmax", "mz_max");
            rename_alias(row, "rt_min", "rtmin");
            rename_alias(row, "rt_max", "rtmax");
            rename_alias(row, "massmin", "mass_min");
            rename_alias(row, "massmax", "mass_max");
            rename_alias(row, "database_id", "database_id");
            set_fragment_columns(row, "ms2_positive", "fragments_mz_pos", "fragments_intensity_pos");
            set_fragment_columns(row, "ms2_negative", "fragments_mz_neg", "fragments_intensity_neg");

            const std::string smiles = text(row, "SMILES");
            const std::string inchi = text(row, "InChI");
            if (smiles.empty() && inchi.empty())
            {
                if (suspect_targets)
                    throw std::invalid_argument("Each suspect target requires SMILES or InChI");
                return;
            }
            const bool supplied_mass = present(row, "mass");
            const double supplied_exact_mass = supplied_mass ? number(row, "mass") : 0.0;
            if (!streamfind::core::vendors::openbabel::openbabel_available())
                throw std::runtime_error("Open Babel is required to validate CSV chemical structures");
            const auto structure = streamfind::core::vendors::openbabel::normalize_structure(smiles, inchi);
            if (!structure.ok)
                throw std::invalid_argument("Invalid target chemical structure: " + structure.error);
            if (supplied_mass)
            {
                constexpr double mass_tolerance_ppm = 5.0;
                const double error_ppm = std::abs(supplied_exact_mass - structure.exact_mass) /
                                         structure.exact_mass * 1e6;
                if (error_ppm > mass_tolerance_ppm)
                {
                    throw std::invalid_argument(
                        "Supplied mass does not agree with the structure-derived exact mass " +
                        std::to_string(structure.exact_mass) + " within " +
                        std::to_string(mass_tolerance_ppm) + " ppm");
                }
            }
            row["SMILES"] = structure.canonical_smiles;
            row["formula"] = structure.formula;
            row["InChI"] = structure.inchi;
            row["InChIKey"] = structure.inchikey;
            row["mass"] = structure.exact_mass;
            if (structure.has_xlogp) row["xLogP"] = structure.xlogp;
        }

        nlohmann::json canonical_row(nlohmann::json row, bool suspect_targets, std::size_t index)
        {
            if (suspect_targets && !present(row, "formula"))
                throw std::invalid_argument("CSV row " + std::to_string(index + 2) + " requires formula");
            normalize_structure(row, suspect_targets);
            if (!present(row, "mass") && !present(row, "mz"))
                throw std::invalid_argument("CSV row " + std::to_string(index + 2) + " requires mass or mz");
            if (suspect_targets)
            {
                if (!present(row, "name")) throw std::invalid_argument("CSV row " + std::to_string(index + 2) + " requires name");
                if (!present(row, "formula")) throw std::invalid_argument("CSV row " + std::to_string(index + 2) + " requires formula");
                if (!present(row, "SMILES") && !present(row, "InChI"))
                    throw std::invalid_argument("CSV row " + std::to_string(index + 2) + " requires SMILES or InChI");
            }
            const char *numeric[] = {"mass", "mass_min", "mass_max", "mz", "mz_min", "mz_max", "rt", "rtmin", "rtmax", "xLogP"};
            for (const auto *key : numeric)
                if (present(row, key)) row[key] = number(row, key);
            if (present(row, "polarity")) row["polarity"] = static_cast<int>(number(row, "polarity"));
            if (present(row, "level")) row["level"] = static_cast<int>(number(row, "level"));
            return row;
        }
    }

    nlohmann::json read_targets_csv(const std::string &path, bool suspect_targets)
    {
        std::ifstream input(path);
        if (!input) throw std::invalid_argument("Cannot open CSV file: " + path);
        std::string line;
        if (!std::getline(input, line)) throw std::invalid_argument("CSV file is empty: " + path);
        const auto header = detail::split_csv_line(line);
        if (header.empty()) throw std::invalid_argument("CSV header is empty: " + path);
        nlohmann::json rows = nlohmann::json::array();
        std::size_t line_number = 1;
        while (std::getline(input, line))
        {
            ++line_number;
            if (detail::trim(line).empty()) continue;
            const auto fields = detail::split_csv_line(line);
            if (fields.size() != header.size())
                throw std::invalid_argument("CSV row " + std::to_string(line_number) + " has a different number of columns than the header");
            nlohmann::json row = nlohmann::json::object();
            static const std::unordered_set<std::string> target_columns = {
                "name", "analysis", "analysis_name", "polarity", "level", "mass", "mass_min", "mass_max",
                "mz", "mz_min", "mz_max", "rt", "rtmin", "rtmax", "rt_min", "rt_max", "formula",
                "smiles", "inchi", "inchikey", "xlogp", "database_id", "ms2_positive", "ms2_negative",
                "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
            for (std::size_t i = 0; i < header.size(); ++i)
            {
                const auto key = detail::lower(detail::trim(header[i]));
                if (!key.empty() && target_columns.contains(key) && !fields[i].empty()) row[key] = fields[i];
            }
            rows.push_back(detail::canonical_row(std::move(row), suspect_targets, rows.size()));
        }
        if (rows.empty()) throw std::invalid_argument("CSV file contains no target rows: " + path);
        return rows;
    }
}
