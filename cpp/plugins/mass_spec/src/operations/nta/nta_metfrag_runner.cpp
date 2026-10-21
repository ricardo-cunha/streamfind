// nta_metfrag_runner.cpp — MetFragCL subprocess runner implementation
//
// Ported operation-faithfully from bindings/r/src/core/nta/nta_metfrag_runner.cpp:
// CSV-format constants, candidate score normalization, and filters are copied
// verbatim; only the plumbing (model structs, tool resolution, persistence) is adapted.

#include "operations/nta/nta_metfrag_runner.hpp"
#include "operations/nta/nta_suspect_screening.hpp"
#include "utils/nta.hpp"
#include "utils/tools_resolver.hpp"
#include "streamfind/core/vendors/openbabel.hpp"
#include "operations/base.hpp"
#include "utils/nta.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace streamfind::mass_spec::nta::metfrag_runner
{
  std::mutex g_openbabel_normalization_mutex;

  const std::vector<std::string> kSupportedMetFragDatabaseTypes = {
    "KEGG",
    "PubChem",
    "ExtendedPubChem",
    "PubChemLite",
    "CompToxCSV",
    "ChemSpiderRest",
    "LocalSDF",
    "LocalPSV",
    "LocalCSV"
  };
  const std::vector<std::string> kSupportedMetFragCandidateWriters = {
    "SDF",
    "XLS",
    "CSV",
    "PSV",
    "FragmentSmilesPSV",
    "LossFragmentSmilesPSV",
    "LossFragmentSmilesExtendedPSV",
    "LossFragmentSmilesCompletePSV",
    "ExtendedPSV",
    "ExtendedXLS",
    "ExtendedFragmentsXLS"
  };

  // ── Internal helpers ──────────────────────────────────────────────────────────

  // Decode a base64 + little-endian float32 encoded string to vector<double>.
  std::vector<double> decode_encoded(const std::string &encoded)
  {
    std::vector<double> out;
    if (encoded.empty())
      return out;
    std::string raw        = ::mass_spec::reader::utils::decode_base64(encoded);
    std::vector<float> fv  = ::mass_spec::reader::utils::decode_little_endian_to_float(raw, 4);
    out.reserve(fv.size());
    for (float f : fv)
      out.push_back(static_cast<double>(f));
    return out;
  }

  // Trim leading/trailing whitespace.
  std::string trim_ws(const std::string &s)
  {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
      return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }

  // Strip leading "mass:" prefixes from a "mass:value;mass:value" string.
  std::string strip_mass_prefixes(const std::string &input)
  {
    if (input.empty()) return {};
    std::vector<std::string> parts;
    std::istringstream ss(input);
    std::string token;
    while (std::getline(ss, token, ';'))
    {
      if (token.empty()) continue;
      std::size_t colon = token.find(':');
      if (colon != std::string::npos)
      {
        std::string val = token.substr(colon + 1);
        if (!val.empty()) parts.push_back(val);
      }
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
      out += (i == 0 ? "" : ";") + parts[i];
    return out;
  }

  std::string to_lower_ascii(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }

  bool is_local_database_type(const std::string &database_type)
  {
    return database_type == "LocalSDF" ||
           database_type == "LocalPSV" ||
           database_type == "LocalCSV";
  }

  bool has_extra_param(
      const std::vector<std::pair<std::string, std::string>> &extra_params,
      const std::string &key)
  {
    for (const auto &entry : extra_params)
    {
      if (entry.first == key && !trim_ws(entry.second).empty())
        return true;
    }
    return false;
  }

  std::string join_strings(
      const std::vector<std::string> &values,
      const std::string &separator)
  {
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
      if (i > 0)
        oss << separator;
      oss << values[i];
    }
    return oss.str();
  }

  std::string join_doubles(
      const std::vector<double> &values,
      const std::string &separator)
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
      if (i > 0)
        oss << separator;
      oss << values[i];
    }
    return oss.str();
  }

  std::string make_run_timestamp()
  {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm time_info{};
#ifdef _WIN32
    localtime_s(&time_info, &now_time);
#else
    localtime_r(&now_time, &time_info);
#endif
    std::ostringstream oss;
    oss << std::put_time(&time_info, "%Y%m%d_%H%M%S");
    return oss.str();
  }

  std::string default_metfrag_run_dir()
  {
    return (fs::path(".") / "log" / "metfrag" / ("run_" + make_run_timestamp())).string();
  }

  std::string default_empty_peak_list_path(const std::string &run_dir)
  {
    return (fs::path(run_dir) / "_metfrag_empty_peaklist.txt").string();
  }

  void ensure_empty_peak_list_file(const std::string &path)
  {
    if (fs::exists(path))
      return;
    std::ofstream f(path);
  }

  // Build a safe filename component from analysis + feature names.
  std::string safe_id(const std::string &a, const std::string &b)
  {
    std::string combined = a + "_" + b;
    std::string out;
    out.reserve(combined.size());
    for (unsigned char c : combined)
      out += (std::isalnum(c) || c == '-' || c == '.') ? static_cast<char>(c) : '_';
    return out;
  }

  // Windows can briefly retain a child-process or antivirus handle after
  // MetFrag exits. Cleanup must never turn that transient condition into a
  // failed screening operation.
  void remove_file_best_effort(const std::string &path)
  {
    if (path.empty())
      return;
    std::string last_error;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
      std::error_code ec;
      if (!fs::exists(path, ec) || fs::remove(path, ec))
        return;
      last_error = ec.message();
      if (attempt < 4)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "[metfrag] cleanup deferred for '" << path << "': "
              << last_error << std::endl;
  }

  // ── File writing ─────────────────────────────────────────────────────────

  void write_peak_list(
      const std::string &path,
      const std::vector<double> &mz,
      const std::vector<double> &intensity)
  {
    std::ofstream f(path);
    for (size_t i = 0; i < mz.size() && i < intensity.size(); ++i)
      f << mz[i] << " " << intensity[i] << "\n";
  }

  void write_params_file(
      const std::string &path,
      const std::string &common_template,
      double precursor_mass,
      int polarity,
      const std::string &ms2_path,
      const std::string &sample_name)
  {
    std::ofstream f(path);
    // MetFrag (Java) requires forward slashes in paths on all platforms.
    auto fwd = [](std::string s) -> std::string {
      for (char &c : s) if (c == '\\') c = '/';
      return s;
    };
    f << common_template;
    f << "PeakListPath = " << fwd(ms2_path) << "\n";
    f << "NeutralPrecursorMass = " << precursor_mass << "\n";
    f << "PrecursorIonMode = " << polarity << "\n";
    f << "IsPositiveIonMode = " << ((polarity > 0) ? "True" : "False") << "\n";
    f << "SampleName = " << sample_name << "\n";
  }

  // ── Subprocess ───────────────────────────────────────────────────────────

  // Run MetFragCL and redirect stdout+stderr to log_path.
  // Returns exit status (0 = success).
  int run_metfrag(
      const std::string &metfrag_path,
      const std::string &java_path,
      const std::string &params_path,
      const std::string &log_path)
  {

    // JAR mode: file ends with .jar (case-insensitive)
    bool is_jar = (metfrag_path.size() > 4) &&
                  (metfrag_path.substr(metfrag_path.size() - 4) == ".jar" ||
                   metfrag_path.substr(metfrag_path.size() - 4) == ".JAR");
#ifdef _WIN32
    std::vector<std::wstring> args;
    auto widen = [](const std::string &value) { return std::wstring(value.begin(), value.end()); };
    const std::wstring executable = widen(is_jar ? java_path : metfrag_path);
    if (is_jar) args = {L"-jar", widen(metfrag_path), widen(params_path)};
    else args = {widen(params_path)};
    std::wstring command = L"\"" + executable + L"\"";
    for (const auto &arg : args) command += L" \"" + arg + L"\"";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    HANDLE output = CreateFileW(widen(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = output; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    if (!created) { CloseHandle(output); return -1; }
    WaitForSingleObject(process.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(output); return static_cast<int>(code);
#else
    std::vector<std::string> values;
    values.push_back(is_jar ? java_path : metfrag_path);
    if (is_jar) values.insert(values.end(), {"-jar", metfrag_path, params_path});
    else values.push_back(params_path);
    std::vector<char *> argv; for (auto &value : values) argv.push_back(value.data()); argv.push_back(nullptr);
    const int output = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666); if (output < 0) return -1;
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
    pid_t pid = 0; const int spawned = posix_spawn(&pid, values.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions); close(output); if (spawned != 0) return -1;
    int status = 0; if (waitpid(pid, &status, 0) < 0) return -1; return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  }

  void forward_metfrag_log(::streamfind::sdk::PluginProjectAccess &access,
                           const std::string &log_path)
  {
    std::ifstream input(log_path);
    std::string line;
    while (std::getline(input, line))
      if (!line.empty()) access.report_progress(0.0, line);
  }

  // ── CSV parsing ───────────────────────────────────────────────────────────

  std::string build_common_params_template(
      const MetFragParams &params,
      const std::string &database_path,
      const std::string &results_dir)
  {
    std::ostringstream oss;
    auto fwd = [](std::string s) -> std::string {
      for (char &c : s) if (c == '\\') c = '/';
      return s;
    };
    auto w = [&](const std::string &k, const std::string &v) {
      oss << k << " = " << v << "\n";
    };

    for (const auto &kv : params.extra_params)
      w(kv.first, kv.second);

    w("MetFragDatabaseType", params.database_type);
    w("DatabaseSearchRelativeMassDeviation", std::to_string(params.ppm));
    w("FragmentPeakMatchRelativeMassDeviation", std::to_string(params.ppmMS2));
    w("FragmentPeakMatchAbsoluteMassDeviation", std::to_string(params.mzrMS2));
    w("MetFragScoreTypes", join_strings(params.score_types, ","));
    w("MetFragScoreWeights", join_doubles(params.score_weights, ","));
    w("MetFragPreProcessingCandidateFilter", join_strings(params.pre_processing_candidate_filter, ","));
    // MetFrag treats an empty filter property as a filter with an invalid
    // empty name.  Omitting the optional property is the documented way to
    // run without post-processing filters.
    if (!params.post_processing_candidate_filter.empty())
      w("MetFragPostProcessingCandidateFilter", join_strings(params.post_processing_candidate_filter, ","));
    w("MetFragCandidateWriter", join_strings(params.candidate_writer, ","));
    w("ResultsPath", fwd(results_dir));
    w("MaximumTreeDepth", std::to_string(params.maximum_tree_depth));
    w("NumberThreads", std::to_string(params.number_threads));
    w("UseSmiles", params.use_smiles ? "True" : "False");

    if (!database_path.empty())
      w("LocalDatabasePath", fwd(database_path));

    return oss.str();
  }

  std::vector<std::string> collect_metfrag_result_files(
      const std::string &results_dir,
      const std::string &sample_name)
  {
    std::vector<std::string> candidates = {
      results_dir + "/" + sample_name + ".csv",
      results_dir + "/" + sample_name + "_1.csv",
      results_dir + "/" + sample_name + ".psv",
      results_dir + "/" + sample_name + "_1.psv"
    };

    try
    {
      for (const auto &entry : fs::directory_iterator(results_dir))
      {
        if (!entry.is_regular_file())
          continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".csv" && ext != ".psv")
          continue;
        std::string stem = entry.path().stem().string();
        if (stem.rfind(sample_name, 0) == 0)
          candidates.push_back(entry.path().string());
      }
    }
    catch (...) {}

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::vector<std::string> existing;
    for (const auto &cand : candidates)
    {
      if (fs::exists(cand))
        existing.push_back(cand);
    }
    return existing;
  }

  std::vector<std::string> split_delimited_line(const std::string &line, char delimiter)
  {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
      char c = line[i];
      if (c == '"')
      {
        if (in_quotes && i + 1 < line.size() && line[i + 1] == '"')
          { field += '"'; ++i; }
        else
          in_quotes = !in_quotes;
      }
      else if (c == delimiter && !in_quotes)
        { fields.push_back(trim_ws(field)); field.clear(); }
      else
        field += c;
    }
    fields.push_back(trim_ws(field));
    return fields;
  }

  std::vector<std::string> split_csv_line(const std::string &line)
  {
    return split_delimited_line(line, ',');
  }

  std::vector<std::string> split_psv_line(const std::string &line)
  {
    return split_delimited_line(line, '|');
  }

  std::string csv_escape(const std::string &value)
  {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
      return value;

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char c : value)
    {
      if (c == '"')
        out += "\"\"";
      else
        out.push_back(c);
    }
    out.push_back('"');
    return out;
  }

  // Case-insensitive column-index lookup.
  int find_col(
      const std::vector<std::string> &headers,
      std::initializer_list<const char *> options)
  {
    for (const char *opt : options)
    {
      std::string o = opt;
      std::transform(o.begin(), o.end(), o.begin(), ::tolower);
      for (size_t i = 0; i < headers.size(); ++i)
      {
        std::string h = headers[i];
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        if (h == o)
          return static_cast<int>(i);
      }
    }
    return -1;
  }

  struct MetFragRow
  {
    std::string name;
    std::string formula;
    std::string SMILES;
    std::string InChI;
    std::string InChIKey;
    std::string identifier;
    double      score       = 0.0;
    double      xLogP       = std::numeric_limits<double>::quiet_NaN();
    double      neutral_mass= std::numeric_limits<double>::quiet_NaN();
    std::string expl_peaks;
    std::string expl_formulas;
    std::string expl_smiles;
    std::string expl_aromatic_smiles;
  };

  const SuspectQuery *find_target_metadata(
      const MetFragRow &row,
      const std::vector<SuspectQuery> &targets)
  {
    for (const auto &target : targets)
    {
      if (!row.InChIKey.empty() && !target.InChIKey.empty() &&
          row.InChIKey == target.InChIKey)
        return &target;
      if (!row.formula.empty() && row.formula == target.formula &&
          std::isfinite(row.neutral_mass) && target.has_mass &&
          std::abs(row.neutral_mass - target.mass) <= 1e-6)
        return &target;
      if (!row.name.empty() && row.name == target.name)
        return &target;
    }
    return nullptr;
  }

  MetFragRow mass_only_row(const SuspectQuery &target)
  {
    MetFragRow row;
    row.name = target.name;
    row.formula = target.formula;
    row.SMILES = target.SMILES;
    row.InChI = target.InChI;
    row.InChIKey = target.InChIKey;

    row.xLogP = target.has_xLogP ? target.xLogP : std::numeric_limits<double>::quiet_NaN();
    row.neutral_mass = target.has_mass ? target.mass : std::numeric_limits<double>::quiet_NaN();
    return row;
  }

  std::vector<MetFragRow> mass_candidates(
      const std::vector<SuspectQuery> &targets,
      double precursor_mass,
      double ppm,
      int top_n)
  {
    std::vector<MetFragRow> rows;
    for (const auto &target : targets)
    {
      if (!target.has_mass || !std::isfinite(target.mass))
        continue;
      const double tolerance = target.mass * ppm / 1e6;
      if (std::abs(precursor_mass - target.mass) <= tolerance)
        rows.push_back(mass_only_row(target));
    }
    std::sort(rows.begin(), rows.end(), [&](const MetFragRow &lhs, const MetFragRow &rhs) {
      return std::abs(lhs.neutral_mass - precursor_mass) < std::abs(rhs.neutral_mass - precursor_mass);
    });
    if (rows.size() > static_cast<std::size_t>(top_n))
      rows.resize(static_cast<std::size_t>(top_n));
    return rows;
  }

  bool normalize_structure_fields(
      std::string &smiles,
      std::string &inchi,
      std::string &inchikey,
      std::string &formula,
      double &mass,
      double &xlogp)
  {
    if (smiles.empty() && inchi.empty())
      return false;
    if (!streamfind::core::vendors::openbabel::openbabel_available())
      return false;

    const streamfind::core::vendors::openbabel::NormalizedStructure normalized =
        streamfind::core::vendors::openbabel::normalize_structure(smiles, inchi);
    if (!normalized.ok)
      return false;

    if (!normalized.canonical_smiles.empty())
      smiles = normalized.canonical_smiles;
    if (!normalized.inchi.empty())
      inchi = normalized.inchi;
    if (!normalized.inchikey.empty())
      inchikey = normalized.inchikey;
    if (!normalized.formula.empty())
      formula = normalized.formula;
    mass = normalized.exact_mass;
    if (normalized.has_xlogp)
      xlogp = normalized.xlogp;
    return true;
  }

  std::string resolve_structure_identifier(
      const std::string &preferred_identifier,
      const std::string &inchikey,
      const std::string &inchi,
      const std::string &smiles,
      const std::string &name,
      std::size_t generated_index)
  {
    if (!preferred_identifier.empty())
      return preferred_identifier;
    if (!inchikey.empty())
      return inchikey;
    if (!inchi.empty())
      return inchi;
    if (!smiles.empty())
      return smiles;
    if (!name.empty())
      return name;
    return "row_" + std::to_string(generated_index);
  }

  // Parse a single (CSV or PSV) file and return MetFragRow rows.
  // Use CSV for standard columns; PSV is used only for SmilesOfExplPeaks supplement.
  std::vector<MetFragRow> parse_metfrag_file(
      const std::string &path,
      bool extract_smiles_only)
  {
    std::vector<MetFragRow> out;
    std::ifstream f(path);
    if (!f.is_open())
      return out;

    std::string header_line;
    if (!std::getline(f, header_line))
      return out;
    bool is_psv = (header_line.find('|') != std::string::npos);
    auto split_fn = is_psv ? split_psv_line : split_csv_line;
    auto headers = split_fn(header_line);

    // PubChem MetFrag output uses IUPACName rather than Name. Keep the
    // generic name columns first, then use the IUPAC name as a fallback.
    int ci_name  = find_col(headers, {"Name", "CompoundName", "compound_name",
                                     "IUPACName", "IUPAC_Name"});
    int ci_form  = find_col(headers, {"MolecularFormula", "formula"});
    int ci_smi   = find_col(headers, {"SMILES", "smiles", "CanonicalSMILES"});
    int ci_inchi = find_col(headers, {"InChI", "inchi1", "StandardInChI"});
    int ci_ikey  = find_col(headers, {"InChIKey", "inchi_key"});
    int ci_id    = find_col(headers, {"Identifier", "PubChemCID", "identifier", "InChIKey"});
    int ci_score = find_col(headers, {"Score", "MetFragScore", "TotalScore", "FinalScore"});
    int ci_xlogp = find_col(headers, {"XLogP", "XLogP3", "LogP", "XLogP-3"});
    int ci_mass  = find_col(headers, {"NeutralMass", "MonoisotopicMass", "ExactMass"});
    int ci_expl  = find_col(headers, {"ExplPeaks", "ExplainedPeaks"});
    int ci_exform= find_col(headers, {"FormulasOfExplPeaks", "ExplPeakFormulas"});
    int ci_exsmi = find_col(headers, {"SmilesOfExplPeaks"});
    int ci_exarosmi = find_col(headers, {"AromaticSmilesOfExplPeaks"});

    auto gf = [](const std::vector<std::string> &row, int idx) -> std::string {
      if (idx < 0 || static_cast<size_t>(idx) >= row.size()) return "";
      return row[idx];
    };

    std::string line;
    while (std::getline(f, line))
    {
      if (trim_ws(line).empty()) continue;
      auto row = split_fn(line);
      MetFragRow r;
      if (!extract_smiles_only)
      {
        r.name        = gf(row, ci_name);
        r.formula     = gf(row, ci_form);
        r.SMILES      = gf(row, ci_smi);
        r.InChI       = gf(row, ci_inchi);
        r.InChIKey    = gf(row, ci_ikey);
        r.identifier = gf(row, ci_id);
        std::string ss = gf(row, ci_score);
        if (!ss.empty()) r.score = std::atof(ss.c_str());
        std::string xs = gf(row, ci_xlogp);
        if (!xs.empty() && xs != "NA") r.xLogP = std::atof(xs.c_str());
        std::string ms = gf(row, ci_mass);
        if (!ms.empty()) r.neutral_mass = std::atof(ms.c_str());
        r.expl_peaks          = gf(row, ci_expl);
        r.expl_formulas       = gf(row, ci_exform);
        const bool complete_structure =
          !r.SMILES.empty() && !r.InChI.empty() && !r.InChIKey.empty() &&
          !r.formula.empty() && std::isfinite(r.neutral_mass);
        if (!complete_structure)
        {
          std::lock_guard<std::mutex> lock(g_openbabel_normalization_mutex);
          normalize_structure_fields(
              r.SMILES,
              r.InChI,
              r.InChIKey,
              r.formula,
              r.neutral_mass,
              r.xLogP);
        }
        r.identifier = resolve_structure_identifier(
            r.identifier,
            r.InChIKey,
            r.InChI,
            r.SMILES,
            r.name,
            out.size() + 1);
      }
      // Always extract SMILES columns (available in both CSV and PSV).
      r.expl_smiles         = gf(row, ci_exsmi);
      r.expl_aromatic_smiles= gf(row, ci_exarosmi);
      out.push_back(std::move(r));
    }
    return out;
  }

  // Parse MetFrag output: use CSV for all standard columns (correctly aligned),
  // then supplement fragment SMILES from PSV (if available).
  std::vector<MetFragRow> parse_metfrag_output(
      const std::string &results_dir,
      const std::string &sample_name)
  {
    std::vector<std::string> candidates = collect_metfrag_result_files(results_dir, sample_name);

    // Find a CSV file — these have correct column alignment.
    std::string csv_path;
    for (const auto &cand : candidates)
    {
      std::string ext = cand.size() > 4 ? cand.substr(cand.size() - 4) : "";
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".csv")
      { csv_path = cand; break; }
    }

    // Find a PSV file — used only for SmilesOfExplPeaks.
    std::string psv_path;
    for (const auto &cand : candidates)
    {
      std::string ext = cand.size() > 4 ? cand.substr(cand.size() - 4) : "";
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".psv")
      { psv_path = cand; break; }
    }

    // Parse CSV for full candidate data.
    std::vector<MetFragRow> out = parse_metfrag_file(csv_path, false);

    // Supplement fragment SMILES from PSV (overwrites empty CSV SMILES).
    if (!psv_path.empty())
    {
      std::vector<MetFragRow> psv_rows = parse_metfrag_file(psv_path, true);
      if (psv_rows.size() == out.size())
      {
        for (std::size_t i = 0; i < out.size(); ++i)
        {
          if (!psv_rows[i].expl_smiles.empty())
            out[i].expl_smiles = psv_rows[i].expl_smiles;
          if (!psv_rows[i].expl_aromatic_smiles.empty())
            out[i].expl_aromatic_smiles = psv_rows[i].expl_aromatic_smiles;
        }
      }
    }

    // Sort descending by score.
    std::stable_sort(out.begin(), out.end(),
        [](const MetFragRow &a, const MetFragRow &b) { return a.score > b.score; });
    return out;
  }

  // ── ExplPeaks parsing ─────────────────────────────────────────────────────
  // MetFrag ExplPeaks format: "mz_intensity;mz_intensity;..."
  // FormulasOfExplPeaks format: "mz:formula;mz:formula;..."

  void parse_expl_peaks(
      const std::string &expl_peaks,
      const std::string &expl_formulas,
      std::vector<double> &mz_out,
      std::vector<double> &intensity_out,
      std::string &formula_out)
  {
    mz_out.clear(); intensity_out.clear(); formula_out.clear();
    if (expl_peaks.empty()) return;

    std::istringstream ss(expl_peaks);
    std::string token;
    while (std::getline(ss, token, ';'))
    {
      if (token.empty()) continue;
      size_t us = token.find('_');
      if (us == std::string::npos) continue;
      mz_out.push_back(std::atof(token.substr(0, us).c_str()));
      intensity_out.push_back(std::atof(token.substr(us + 1).c_str()));
    }

    if (!expl_formulas.empty())
    {
      std::vector<std::string> parts;
      std::istringstream fs(expl_formulas);
      std::string ft;
      while (std::getline(fs, ft, ';'))
      {
        size_t colon = ft.find(':');
        if (colon != std::string::npos)
        {
          std::string form = ft.substr(colon + 1);
          if (!form.empty()) parts.push_back(form);
        }
      }
      for (size_t i = 0; i < parts.size(); ++i)
        formula_out += (i == 0 ? "" : ";") + parts[i];
    }
  }

  // ── Cosine similarity ─────────────────────────────────────────────────────

  double cosine_similarity(
      const std::vector<double> &db_mz,
      const std::vector<double> &db_int,
      const std::vector<double> &exp_mz,
      const std::vector<double> &exp_int,
      double ppm_ms2,
      double mzr_ms2,
      int &shared_out)
  {
    shared_out = 0;
    if (db_mz.empty() || exp_mz.empty()) return 0.0;

    std::vector<double> i_db, i_exp;
    i_db.reserve(db_mz.size());
    i_exp.reserve(db_mz.size());

    for (size_t z = 0; z < db_mz.size(); ++z)
    {
      double tol  = std::max(db_mz[z] * ppm_ms2 / 1e6, mzr_ms2);
      double lo   = db_mz[z] - tol;
      double hi   = db_mz[z] + tol;
      int    best = -1;
      double best_err = std::numeric_limits<double>::max();
      for (size_t k = 0; k < exp_mz.size(); ++k)
        if (exp_mz[k] >= lo && exp_mz[k] <= hi)
        {
          double err = std::abs(exp_mz[k] - db_mz[z]);
          if (err < best_err) { best_err = err; best = static_cast<int>(k); }
        }
      if (best >= 0) { i_db.push_back(db_int[z]); i_exp.push_back(exp_int[best]); ++shared_out; }
    }

    if (shared_out == 0) return 0.0;

    double max_db  = *std::max_element(i_db.begin(), i_db.end());
    double max_exp = *std::max_element(i_exp.begin(), i_exp.end());
    if (max_db <= 0.0 || max_exp <= 0.0) return 0.0;

    double dot = 0.0, mag_db = 0.0, mag_exp = 0.0;
    for (size_t k = 0; k < i_db.size(); ++k)
    {
      double di = i_db[k] / max_db, ei = i_exp[k] / max_exp;
      dot += di * ei; mag_db += di * di; mag_exp += ei * ei;
    }
    if (mag_db <= 0.0 || mag_exp <= 0.0) return 0.0;
    return std::round(dot / (std::sqrt(mag_db) * std::sqrt(mag_exp)) * 10000.0) / 10000.0;
  }

  // ── LocalCSV column normalizer ───────────────────────────────────────────
  //
  // MetFrag's LocalCSVDatabase requires specific column names:
  //   Identifier, MonoisotopicMass, MolecularFormula, SMILES, InChI, InChIKey, Name
  //
  // User-supplied CSVs often use: name, mass, formula, smiles, inchi, inchikey, ...
  // This function rewrites a normalised copy in run_dir when the columns differ,
  // and returns the path to use (original if already correct, new copy otherwise).
  std::string normalize_localcsv_database(
      const std::string &db_path,
      const std::string &run_dir_path,
      bool comp_tox_database = false)
  {
    if (db_path.empty()) return db_path;

    std::ifstream in(db_path);
    if (!in.is_open())
    {
      std::cerr << "[metfrag] LocalCSV database not found: " << db_path << "\n";
      return db_path;
    }

    // Read header line.
    std::string header_line;
    if (!std::getline(in, header_line))
      return db_path;

    std::vector<std::string> cols = split_csv_line(header_line);
    if (cols.empty()) return db_path;

    // Case-insensitive column index lookup.
    auto find_col = [&](const std::vector<std::string> &candidates) -> int
    {
      for (const auto &cand : candidates)
      {
        std::string lc_cand = cand;
        std::transform(lc_cand.begin(), lc_cand.end(), lc_cand.begin(), ::tolower);
        for (int i = 0; i < static_cast<int>(cols.size()); ++i)
        {
          std::string lc_col = cols[i];
          std::transform(lc_col.begin(), lc_col.end(), lc_col.begin(), ::tolower);
          if (lc_col == lc_cand) return i;
        }
      }
      return -1;
    };

    // Rename map: MetFrag target → accepted user column names (first match wins).
    struct RenameRule { std::string target; std::vector<std::string> aliases; };
    const std::vector<RenameRule> rules = {
      { "Identifier",       comp_tox_database
          ? std::vector<std::string>{ "DTXCID_INDIVIDUAL_COMPONENT", "DTXCID", "Identifier", "identifier", "id" }
          : std::vector<std::string>{ "Identifier", "identifier", "id" } },
      { "MonoisotopicMass", comp_tox_database
          ? std::vector<std::string>{ "MONOISOTOPIC_MASS_DTXCID", "MonoisotopicMass", "mass" }
          : std::vector<std::string>{ "MonoisotopicMass", "mass" } },
      { "MolecularFormula", comp_tox_database
          ? std::vector<std::string>{ "FORMULA_INDIVIDUAL_COMPONENT", "MolecularFormula", "formula" }
          : std::vector<std::string>{ "MolecularFormula", "formula" } },
      { "SMILES",           comp_tox_database
          ? std::vector<std::string>{ "SMILES_INDIVIDUAL_COMPONENT" }
          : std::vector<std::string>{ "SMILES", "smiles", "Smiles" } },
      { "InChI",            comp_tox_database
          ? std::vector<std::string>{ "INCHI_STRING_DTXCID", "InChI", "inchi", "Inchi" }
          : std::vector<std::string>{ "InChI", "inchi", "Inchi" } },
      { "InChIKey",         comp_tox_database
          ? std::vector<std::string>{ "INCHIKEY_DTXCID", "InChIKey", "inchikey", "Inchikey" }
          : std::vector<std::string>{ "InChIKey", "inchikey", "Inchikey" } },
      { "Name",             comp_tox_database
          ? std::vector<std::string>{ "PREFERRED_NAME_DTXSID", "Name", "name", "CompoundName", "compound_name" }
          : std::vector<std::string>{ "Name", "name", "CompoundName", "compound_name" } }
    };

    struct OutputColumn
    {
      std::string target;
      int source_index = -1;
    };
    std::vector<OutputColumn> output_columns;
    output_columns.reserve(rules.size());
    for (const auto &rule : rules)
    {
      int idx = find_col(rule.aliases);
      output_columns.push_back({rule.target, idx});
    }

    int id_idx   = find_col(rules[0].aliases);
    int mass_idx = find_col(rules[1].aliases);
    int formula_idx = find_col(rules[2].aliases);
    int smiles_idx = find_col(rules[3].aliases);
    int inchi_idx = find_col(rules[4].aliases);
    int ikey_idx = find_col(rules[5].aliases);
    int name_idx = find_col(rules[6].aliases);
    int xlogp_idx = find_col({ "XLogP", "xLogP", "xlogp", "XLogP3", "LogP", "XLogP-3" });
    std::string out_path = run_dir_path + "/metfrag_localcsv_normalized.csv";
    std::ofstream out(out_path);
    if (!out.is_open())
    {
      std::cerr << "[metfrag] Cannot write normalised LocalCSV to: " << out_path << "\n";
      return db_path;
    }

    // Write reduced header used by MetFrag plus optional XLogP passthrough.
    for (size_t i = 0; i < output_columns.size(); ++i)
    {
      if (i > 0) out << ',';
      out << output_columns[i].target;
    }
    if (xlogp_idx >= 0) out << ",XLogP";
    out << '\n';

    // Stream data rows into the reduced schema.
    std::string row;
    std::size_t generated_identifier_index = 0;
    std::size_t excluded_incomplete = 0;
    std::size_t excluded_duplicate = 0;
    std::unordered_set<std::string> seen_identifiers;
    while (std::getline(in, row))
    {
      std::vector<std::string> fields = split_csv_line(row);
      if (fields.empty()) continue;

      auto field_at = [&](int idx) -> std::string
      {
        if (idx < 0 || static_cast<int>(fields.size()) <= idx) return "";
        return fields[static_cast<std::size_t>(idx)];
      };

      ++generated_identifier_index;

      std::string name = field_at(name_idx);
      std::string smiles = field_at(smiles_idx);
      std::string inchi = field_at(inchi_idx);
      std::string inchikey = field_at(ikey_idx);
      std::string formula = field_at(formula_idx);
      std::string identifier = field_at(id_idx);

      if (comp_tox_database)
      {
        // CompTox rows without a stable component identifier, name, neutral
        // mass, formula, or individual-component SMILES cannot be queried or
        // represented reliably by MetFrag and are excluded up front.
        if (identifier.empty() || name.empty() || formula.empty() || smiles.empty())
        {
          ++excluded_incomplete;
          continue;
        }
        const std::string mass_value = field_at(mass_idx);
        char *mass_end = nullptr;
        const double parsed_mass = std::strtod(mass_value.c_str(), &mass_end);
        if (mass_value.empty() || mass_end == mass_value.c_str() || !std::isfinite(parsed_mass))
        {
          ++excluded_incomplete;
          continue;
        }
        if (!seen_identifiers.insert(identifier).second)
        {
          ++excluded_duplicate;
          continue;
        }
      }

      double mass = std::numeric_limits<double>::quiet_NaN();
      const std::string mass_raw = field_at(mass_idx);
      if (!mass_raw.empty() && mass_raw != "NA")
        mass = std::atof(mass_raw.c_str());

      double xlogp = std::numeric_limits<double>::quiet_NaN();
      const std::string xlogp_raw = field_at(xlogp_idx);
      if (!xlogp_raw.empty() && xlogp_raw != "NA")
        xlogp = std::atof(xlogp_raw.c_str());

      // PubChemLite and other canonical exports already contain all required
      // structure fields. Avoid invoking Open Babel for every row in those
      // files; normalization is only needed when a required field is absent.
      const bool complete_structure =
          !smiles.empty() && !inchi.empty() && !inchikey.empty() &&
          !formula.empty() && std::isfinite(mass);
      if (!complete_structure)
        normalize_structure_fields(smiles, inchi, inchikey, formula, mass, xlogp);
      identifier = resolve_structure_identifier(
          identifier,
          inchikey,
          inchi,
          smiles,
          name,
          generated_identifier_index);

      auto normalized_value = [&](const std::string &target) -> std::string
      {
        if (target == "Identifier")
          return identifier;
        if (target == "MonoisotopicMass")
        {
          if (std::isnan(mass))
            return "";
          std::ostringstream oss;
          oss << std::fixed << std::setprecision(10) << mass;
          return oss.str();
        }
        if (target == "MolecularFormula")
          return formula;
        if (target == "SMILES")
          return smiles;
        if (target == "InChI")
          return inchi;
        if (target == "InChIKey")
          return inchikey;
        if (target == "Name")
          return name;
        return "";
      };

      for (size_t i = 0; i < output_columns.size(); ++i)
      {
        if (i > 0) out << ',';
        std::string value = normalized_value(output_columns[i].target);
        out << csv_escape(value);
      }
      if (xlogp_idx >= 0)
      {
        out << ',';
        if (!std::isnan(xlogp))
        {
          std::ostringstream oss;
          oss << std::fixed << std::setprecision(6) << xlogp;
          out << csv_escape(oss.str());
        }
      }
      out << '\n';
    }

    if (comp_tox_database)
      std::cerr << "[metfrag] CompTox normalization excluded " << excluded_incomplete
                << " incomplete rows and " << excluded_duplicate
                << " duplicate identifiers.\n";

    return out_path;
  }

  // ── Public implementation ─────────────────────────────────────────────────────

  std::vector<std::string> supported_database_types()
  {
    return kSupportedMetFragDatabaseTypes;
  }

  std::string canonicalize_database_type(const std::string &database_type)
  {
    const std::string needle = to_lower_ascii(trim_ws(database_type));
    for (const auto &value : kSupportedMetFragDatabaseTypes)
    {
      if (to_lower_ascii(value) == needle)
        return value;
    }

    std::ostringstream oss;
    oss << "Unsupported MetFrag database_type '" << database_type << "'. Supported values are: ";
    for (size_t i = 0; i < kSupportedMetFragDatabaseTypes.size(); ++i)
    {
      if (i > 0)
        oss << ", ";
      oss << kSupportedMetFragDatabaseTypes[i];
    }
    throw std::invalid_argument(oss.str());
  }

  MetFragParams canonicalize_and_validate_params(const MetFragParams &params)
  {
    MetFragParams out = params;
    out.database_type = canonicalize_database_type(params.database_type);

    // Validate metfrag_path
    if (out.metfrag_path.empty())
    {
      throw std::invalid_argument(
        "MetFrag 'metfrag_path' is empty. Provide the path to MetFragCL.jar "
        "(or a native MetFragCL executable)."
      );
    }
    if (!fs::exists(out.metfrag_path))
    {
      // If it doesn't exist as a file, it might be a command in PATH;
      // check with which/where to give a clearer error.
      throw std::invalid_argument(
        "MetFrag executable not found at 'metfrag_path': " + out.metfrag_path
      );
    }

    // Validate java_path when MetFrag is a .jar
    bool is_jar = (out.metfrag_path.size() > 4) &&
                  (out.metfrag_path.substr(out.metfrag_path.size() - 4) == ".jar" ||
                   out.metfrag_path.substr(out.metfrag_path.size() - 4) == ".JAR");
    if (is_jar)
    {
      if (out.java_path.empty())
      {
        throw std::invalid_argument(
          "MetFrag is a .jar file but 'java_path' is empty. "
          "Set java_path to 'java' (system PATH) or the full path to java.exe."
        );
      }
      if (out.java_path != "java" && out.java_path.find("java") == std::string::npos)
      {
        throw std::invalid_argument(
          "MetFrag 'java_path' does not appear to point to a Java executable: " + out.java_path
        );
      }
    }

    if (out.score_types.empty())
      throw std::invalid_argument("MetFrag score_types must not be empty.");
    if (out.score_types.size() != out.score_weights.size())
      throw std::invalid_argument("MetFrag score_types and score_weights must have the same length.");
    // MetFrag cannot accept an empty pre-processing filter property. Older
    // project revisions can nevertheless contain an explicit [] value, which
    // overrides the parameter default during JSON deserialization. Treat that
    // value as the operation default so those projects remain runnable.
    if (out.pre_processing_candidate_filter.empty())
      out.pre_processing_candidate_filter = {"UnconnectedCompoundFilter", "IsotopeFilter"};
    if (out.candidate_writer.empty())
      throw std::invalid_argument("MetFrag candidate_writer must not be empty.");
    for (const auto &writer : out.candidate_writer)
    {
      if (std::find(kSupportedMetFragCandidateWriters.begin(), kSupportedMetFragCandidateWriters.end(), writer) ==
          kSupportedMetFragCandidateWriters.end())
      {
        throw std::invalid_argument("Unsupported MetFrag candidate_writer '" + writer + "'.");
      }
    }
    if (out.include_fragment_smiles &&
        std::find(out.candidate_writer.begin(), out.candidate_writer.end(), "FragmentSmilesPSV") == out.candidate_writer.end())
      throw std::invalid_argument("MetFrag candidate_writer must include 'FragmentSmilesPSV' for streamfind result parsing.");
    if (out.maximum_tree_depth < 1)
      throw std::invalid_argument("MetFrag maximum_tree_depth must be at least 1.");
    if (out.number_threads < 1)
      throw std::invalid_argument("MetFrag number_threads must be at least 1.");
    if (out.feature_parallelism < 1)
      throw std::invalid_argument("MetFrag feature_parallelism must be at least 1.");

    if (is_local_database_type(out.database_type))
    {
      if (trim_ws(out.database_path).empty())
      {
        throw std::invalid_argument(
          "MetFrag database_type '" + out.database_type +
          "' requires a non-empty database_path."
        );
      }
      if (!fs::exists(out.database_path))
      {
        throw std::invalid_argument(
          "MetFrag local database file does not exist: " + out.database_path
        );
      }
    }

    if (out.database_type == "ChemSpiderRest" &&
        !has_extra_param(out.extra_params, "ChemSpiderToken"))
    {
      throw std::invalid_argument(
        "MetFrag database_type 'ChemSpiderRest' requires extra_params[['ChemSpiderToken']]."
      );
    }

    return out;
  }

  std::string resolve_run_dir(const MetFragParams &params)
  {
    return params.run_dir.empty() ? default_metfrag_run_dir() : params.run_dir;
  }

  void metfrag_screening_impl(
    ::streamfind::sdk::PluginProjectAccess &access,
    NtaProjectData &nta_data,
    const MetFragParams &p)
  {
    const MetFragParams params = canonicalize_and_validate_params(p);
    const auto &analysis_names = nta_data.analysis_names();
    auto &feature_buffers = nta_data.feature_buffers();
    auto &suspect_buffers = nta_data.suspect_buffers();
    const size_t n_ana = analysis_names.size();

    // Ensure run directory exists.
    std::string run_dir = resolve_run_dir(params);
    access.report_progress(0.0, "MetFrag screening started.");
    try { fs::create_directories(run_dir); }
    catch (const std::exception &e)
    {
      std::cerr << "[metfrag_runner] Failed to create run_dir '" << run_dir << "': " << e.what() << "\n";
    }
    std::cerr << "[metfrag_runner] run_dir: " << run_dir << std::endl;

    // Normalise local database column names once before the feature loop.
    std::string effective_db_path = params.database_path;
    if (!effective_db_path.empty())
    {
      if (params.database_type == "LocalCSV" || params.comp_tox_database)
        effective_db_path = normalize_localcsv_database(
            effective_db_path, run_dir, params.comp_tox_database);
    }

    const std::string common_params_template =
      build_common_params_template(params, effective_db_path, run_dir);
    const std::string shared_empty_peak_list = default_empty_peak_list_path(run_dir);
    ensure_empty_peak_list_file(shared_empty_peak_list);

    // Keep temporary MetFrag inputs/results until every child process and
    // worker future has completed. This avoids Windows sharing violations
    // caused by delayed release of Java/antivirus file handles.
    std::vector<std::string> deferred_cleanup;
    std::mutex deferred_cleanup_mutex;
    auto defer_cleanup = [&](const std::string &path)
    {
      if (path.empty() || path == shared_empty_peak_list)
        return;
      std::lock_guard<std::mutex> lock(deferred_cleanup_mutex);
      deferred_cleanup.push_back(path);
    };

    // Reset suspects for all analyses. Feature work is independent, but the
    // columnar suspect buffers and workflow progress reporter are updated only
    // by this coordinator thread after each worker result is complete.
    std::size_t total_completed_work = 0;
    for (size_t ai = 0; ai < n_ana; ++ai)
      suspect_buffers[ai] = ::streamfind::mass_spec::nta::api::NTA_SUSPECTS();

    struct FeatureResult
    {
      std::vector<::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW> suspects;
      std::size_t feature_index = 0;
      std::size_t candidate_count = 0;
      int status = 0;
      bool cache_hit = false;
    };

    std::unordered_map<std::string, std::vector<MetFragRow>> candidate_cache;
    std::mutex candidate_cache_mutex;

    auto query_cache_key = [&](const std::string &database_path,
                               double precursor_mass,
                               int polarity,
                               const std::string &ms2_mz,
                               const std::string &ms2_intensity)
    {
      std::ostringstream key;
      key << params.database_type << '|' << database_path << '|'
          << std::setprecision(17) << precursor_mass << '|'
          << polarity << '|' << params.ppm << '|' << params.ppmMS2 << '|'
          << params.mzrMS2 << '|' << params.maximum_tree_depth << '|'
          << params.use_smiles << '|' << ms2_mz << '|' << ms2_intensity;
      return key.str();
    };

    std::size_t total_work = 0;
    for (size_t ai = 0; ai < n_ana; ++ai)
    {
      const auto &feats = feature_buffers[ai];
      for (int fi = 0; fi < feats.size(); ++fi)
      {
        if (!params.filtered && feats.filtered[fi])
          continue;
        const double precursor_mass = feats.mass[fi] > 0.0f
            ? static_cast<double>(feats.mass[fi])
            : (feats.mz[fi] > 0.0f
                ? static_cast<double>(feats.mz[fi]) - feats.polarity[fi] * 1.007276
                : std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(precursor_mass))
          ++total_work;
      }
    }

    access.report_progress(0.0,
      "MetFrag prepared " + std::to_string(total_work) +
        " features; feature workers=" + std::to_string(params.feature_parallelism) +
        ", MetFrag threads=" + std::to_string(params.number_threads) +
        ", cache=" + (params.cache_candidates ? "on" : "off") +
        ", mass-only=always");

    for (size_t ai = 0; ai < n_ana; ++ai)
    {
      const std::string &ana = analysis_names[ai];
      const auto &feats = feature_buffers[ai];
      const int n_feat = feats.size();
      std::size_t analysis_work = 0;
      for (int fi = 0; fi < n_feat; ++fi)
      {
        if (!params.filtered && feats.filtered[fi])
          continue;
        const double precursor_mass = feats.mass[fi] > 0.0f
            ? static_cast<double>(feats.mass[fi])
            : (feats.mz[fi] > 0.0f
                ? static_cast<double>(feats.mz[fi]) - feats.polarity[fi] * 1.007276
                : std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(precursor_mass))
          ++analysis_work;
      }
      if (analysis_work == 0)
        continue;

      std::cerr << ai + 1 << "/" << n_ana << " MetFrag screening: " << ana
                << " (" << analysis_work << " eligible features)" << std::endl;
      access.report_progress(
        total_work == 0 ? 0.0 : static_cast<double>(total_completed_work) / static_cast<double>(total_work),
        "MetFrag reading analysis " + std::to_string(ai + 1) + "/" +
          std::to_string(n_ana) + ": " + ana + " (" +
          std::to_string(analysis_work) + " eligible features)");

      auto process_feature = [&](int fi) -> FeatureResult
      {
        FeatureResult result;
        result.feature_index = static_cast<std::size_t>(fi);

        std::vector<double> ms2_mz = decode_encoded(feats.ms2_mz[fi]);
        std::vector<double> ms2_int = decode_encoded(feats.ms2_intensity[fi]);
        const double precursor_mass = feats.mass[fi] > 0.0f
            ? static_cast<double>(feats.mass[fi])
            : static_cast<double>(feats.mz[fi]) - feats.polarity[fi] * 1.007276;
        const bool has_ms2 = !ms2_mz.empty();
        // Database-backed MetFrag searches can also be used as precursor-mass
        // candidate retrieval when a feature has no experimental MS2. The
        // shared empty peak list keeps the normal MetFrag database path and
        // candidate writers in use; the resulting candidates are assigned
        // level 4 below because no fragment or RT evidence is available.
        const bool run_mass_only = !has_ms2 &&
          (params.database_type == "PubChem" ||
           params.database_type == "ExtendedPubChem" ||
           params.database_type == "LocalCSV" ||
           params.comp_tox_database);
        const bool run_metfrag_search = has_ms2 || run_mass_only;
        // Include the row index: feature labels are user/data-derived and may
        // repeat within an analysis, especially after gap filling.
        const std::string sid = safe_id(ana, feats.feature[fi]) + "_" + std::to_string(fi);
        const std::string ms2_path = has_ms2
            ? (run_dir + "/ms2_" + sid + ".txt") : shared_empty_peak_list;
        const std::string params_path = run_dir + "/metfrag_" + sid + ".params";
        const std::string log_path = run_dir + "/metfrag_" + sid + ".log";
        const std::string sample_name = "metfrag_" + sid;

        std::vector<MetFragRow> rows;
        const std::string cache_key = query_cache_key(
          effective_db_path, precursor_mass, feats.polarity[fi],
          feats.ms2_mz[fi], feats.ms2_intensity[fi]);
        if (params.cache_candidates && run_metfrag_search)
        {
          std::lock_guard<std::mutex> lock(candidate_cache_mutex);
          const auto cached = candidate_cache.find(cache_key);
          if (cached != candidate_cache.end())
          {
            rows = cached->second;
            result.cache_hit = true;
          }
        }

        if (!result.cache_hit && run_metfrag_search)
        {
          write_peak_list(ms2_path, ms2_mz, ms2_int);
          write_params_file(params_path, common_params_template, precursor_mass,
                            feats.polarity[fi], ms2_path, sample_name);
          result.status = run_metfrag(params.metfrag_path, params.java_path,
                                      params_path, log_path);
          rows = parse_metfrag_output(run_dir, sample_name);
          if (params.cache_candidates)
          {
            std::lock_guard<std::mutex> lock(candidate_cache_mutex);
            candidate_cache.emplace(cache_key, rows);
          }
        }

        if (rows.empty())
          rows = mass_candidates(params.suspect_targets, precursor_mass, params.ppm, params.top_n);

        const std::vector<std::string> csv_paths = collect_metfrag_result_files(run_dir, sample_name);
        if (rows.empty())
        {
          if (has_ms2) defer_cleanup(ms2_path);
          defer_cleanup(params_path);
          for (const auto &csv_path : csv_paths) defer_cleanup(csv_path);
          if (result.status == 0) defer_cleanup(log_path);
          return result;
        }

        int rank = 1;
        for (const MetFragRow &row : rows)
        {
          if (rank > params.top_n)
            break;
          std::vector<double> db_mz, db_int;
          std::string db_form;
          parse_expl_peaks(row.expl_peaks, row.expl_formulas, db_mz, db_int, db_form);
          std::vector<float> db_mzf;
          db_mzf.reserve(db_mz.size());
          for (const double value : db_mz) db_mzf.push_back(static_cast<float>(value));
          std::vector<float> db_intf;
          db_intf.reserve(db_int.size());
          for (const double value : db_int) db_intf.push_back(static_cast<float>(value));
          const std::string db_ms2_mz_enc = ::streamfind::mass_spec::nta::utils::encode_floats_base64(db_mzf);
          const std::string db_ms2_int_enc = ::streamfind::mass_spec::nta::utils::encode_floats_base64(db_intf);

          const SuspectQuery *target = find_target_metadata(row, params.suspect_targets);
          // Every candidate source must use the same isotope scorer. Database
          // candidates are normally absent from suspect_targets, so construct
          // equivalent metadata from the MetFrag row. Some database exports
          // omit MolecularFormula but include a structure; derive the formula
          // before scoring so all database modes receive MS1 isotope values.
          SuspectQuery candidate_target;
          const SuspectQuery *isotope_target = target;
          std::string display_name = row.name;
          const bool pubchem_database = params.database_type == "PubChem" ||
              params.database_type == "ExtendedPubChem";
          if (pubchem_database && !row.identifier.empty())
          {
            display_name = display_name.empty()
                ? "PubChem CID " + row.identifier
                : display_name + " | " + row.identifier;
          }
          else if (display_name.empty())
          {
            display_name = row.identifier;
          }
          if (isotope_target == nullptr)
          {
            candidate_target.name = display_name;
            candidate_target.formula = row.formula;
            candidate_target.SMILES = row.SMILES;
            candidate_target.InChI = row.InChI;
            candidate_target.InChIKey = row.InChIKey;
            if (std::isfinite(row.neutral_mass))
            {
              candidate_target.mass = row.neutral_mass;
              candidate_target.has_mass = true;
            }
            if (candidate_target.formula.empty() &&
                (!candidate_target.SMILES.empty() || !candidate_target.InChI.empty()))
            {
              std::lock_guard<std::mutex> lock(g_openbabel_normalization_mutex);
              double mass = candidate_target.has_mass
                  ? candidate_target.mass : std::numeric_limits<double>::quiet_NaN();
              double xlogp = std::numeric_limits<double>::quiet_NaN();
              normalize_structure_fields(candidate_target.SMILES,
                                         candidate_target.InChI,
                                         candidate_target.InChIKey,
                                         candidate_target.formula,
                                         mass, xlogp);
              if (!candidate_target.has_mass && std::isfinite(mass))
              {
                candidate_target.mass = mass;
                candidate_target.has_mass = true;
              }
            }
            isotope_target = candidate_target.formula.empty() ? nullptr : &candidate_target;
          }
          else if (isotope_target->formula.empty() &&
                   (!isotope_target->SMILES.empty() || !isotope_target->InChI.empty()))
          {
            candidate_target = *isotope_target;
            std::lock_guard<std::mutex> lock(g_openbabel_normalization_mutex);
            double mass = candidate_target.has_mass
                ? candidate_target.mass : std::numeric_limits<double>::quiet_NaN();
            double xlogp = candidate_target.has_xLogP
                ? candidate_target.xLogP : std::numeric_limits<double>::quiet_NaN();
            normalize_structure_fields(candidate_target.SMILES,
                                       candidate_target.InChI,
                                       candidate_target.InChIKey,
                                       candidate_target.formula,
                                       mass, xlogp);
            isotope_target = candidate_target.formula.empty() ? isotope_target : &candidate_target;
          }
          const auto isotope = isotope_target
              ? ::streamfind::mass_spec::nta::suspect_screening::matches_isotope_pattern(
                    *isotope_target, feats, static_cast<std::size_t>(fi), params.isotopePpm)
              : ::streamfind::mass_spec::nta::suspect_screening::IsotopeMatch{};
          int shared = 0;
          double cosine = 0.0;
          if (!db_mz.empty() && !ms2_mz.empty())
            cosine = cosine_similarity(db_mz, db_int, ms2_mz, ms2_int,
                                       params.ppmMS2, params.mzrMS2, shared);
          const double database_mass = std::isfinite(row.neutral_mass)
              ? row.neutral_mass
              : (target && target->has_mass ? target->mass : std::numeric_limits<double>::quiet_NaN());
          double error_mass = std::numeric_limits<double>::quiet_NaN();
          if (std::isfinite(database_mass) && precursor_mass > 0.0)
            error_mass = std::round(((precursor_mass - database_mass) / precursor_mass) * 1e7) / 10.0;
          const double db_rt_val = target && target->has_rt
              ? target->rt : std::numeric_limits<double>::quiet_NaN();
          const double error_rt = std::isfinite(db_rt_val)
              ? static_cast<double>(feats.rt[fi]) - db_rt_val
              : std::numeric_limits<double>::quiet_NaN();
          const bool rt_match = std::isfinite(db_rt_val) &&
            std::abs(static_cast<double>(feats.rt[fi]) - db_rt_val) <= params.sec;

          ::streamfind::mass_spec::nta::api::NTA_SUSPECT_ROW s;
          s.analysis = ana;
          s.feature = feats.feature[fi];
          s.feature_group = fi < static_cast<int>(feats.feature_group.size())
              ? feats.feature_group[fi] : std::string{};
          s.candidate_rank = rank;
          s.name = display_name;
          s.polarity = feats.polarity[fi];
          s.db_mass = database_mass;
          s.exp_mass = precursor_mass;
          s.error_mass = error_mass;
          s.db_rt = db_rt_val;
          s.exp_rt = static_cast<double>(feats.rt[fi]);
          s.error_rt = error_rt;
          s.intensity = static_cast<double>(feats.intensity[fi]);
          s.area = static_cast<double>(feats.area[fi]);
          s.id_level = (shared > 0 || rt_match) ? 3 : 4;
          s.score = row.score;
          s.shared_fragments = shared;
          s.cosine_similarity = cosine;
          s.formula = row.formula;
          s.SMILES = row.SMILES;
          s.InChI = row.InChI;
          s.InChIKey = row.InChIKey;
          s.xLogP = row.xLogP;
          s.db_ms2_size = static_cast<int>(db_mz.size());
          s.db_ms2_mz = db_ms2_mz_enc;
          s.db_ms2_intensity = db_ms2_int_enc;
          s.db_ms2_formula = db_form;
          s.db_ms2_smiles = strip_mass_prefixes(row.expl_smiles);
          s.exp_ms2_size = feats.ms2_size[fi];
          s.exp_ms2_mz = feats.ms2_mz[fi];
          s.exp_ms2_intensity = feats.ms2_intensity[fi];
          s.isotope_theoretical_peaks = isotope.theoretical_peaks;
          s.isotope_matched_peaks = isotope.matched_peaks;
          s.isotope_similarity = isotope.similarity;
          s.isotope_match = isotope.evaluated && isotope.matched_peaks > 0;
          result.suspects.push_back(std::move(s));
          ++rank;
        }

        for (const auto &csv_path : csv_paths)
        {
          std::string ext = csv_path.size() > 4 ? csv_path.substr(csv_path.size() - 4) : "";
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".csv") defer_cleanup(csv_path);
        }
        if (has_ms2) defer_cleanup(ms2_path);
        defer_cleanup(params_path);
        if (result.status == 0) defer_cleanup(log_path);
        result.candidate_count = result.suspects.size();
        return result;
      };

      std::vector<std::future<FeatureResult>> pending;
      pending.reserve(static_cast<std::size_t>(params.feature_parallelism));
      std::size_t completed_work = 0;
      const std::size_t log_interval = std::max<std::size_t>(1, analysis_work / 20);
      auto collect_pending = [&]()
      {
        for (auto &future : pending)
        {
          FeatureResult result = future.get();
          ++completed_work;
          ++total_completed_work;
          for (auto &suspect : result.suspects)
            suspect_buffers[ai].append(std::move(suspect));
          if (completed_work == analysis_work ||
              completed_work % log_interval == 0)
          {
            const std::size_t global_completed = total_completed_work;
            const double progress = total_work == 0 ? 1.0
              : static_cast<double>(global_completed) / static_cast<double>(total_work);
            access.report_progress(progress,
              "MetFrag " + ana + " completed " + std::to_string(completed_work) +
                "/" + std::to_string(analysis_work) + " features; " +
                std::to_string(result.suspects.size()) + " candidates" +
                (result.cache_hit ? " (cache hit)" : ""));
          }
        }
        pending.clear();
      };

      for (int fi = 0; fi < n_feat; ++fi)
      {
        if (!params.filtered && feats.filtered[fi])
          continue;
        const double precursor_mass = feats.mass[fi] > 0.0f
            ? static_cast<double>(feats.mass[fi])
            : (feats.mz[fi] > 0.0f
                ? static_cast<double>(feats.mz[fi]) - feats.polarity[fi] * 1.007276
                : std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(precursor_mass))
          continue;
        pending.emplace_back(std::async(std::launch::async, process_feature, fi));
        if (pending.size() >= static_cast<std::size_t>(params.feature_parallelism))
          collect_pending();
      }
      if (!pending.empty())
        collect_pending();

      std::cerr << "  Found " << suspect_buffers[ai].size()
                << " suspect(s) in " << ana << std::endl;
    }

    std::sort(deferred_cleanup.begin(), deferred_cleanup.end());
    deferred_cleanup.erase(
      std::unique(deferred_cleanup.begin(), deferred_cleanup.end()),
      deferred_cleanup.end());
    for (const auto &path : deferred_cleanup)
      remove_file_best_effort(path);
    remove_file_best_effort(shared_empty_peak_list);
  }

} // namespace streamfind::mass_spec::nta::metfrag_runner
#include "utils/nta.hpp"
namespace streamfind::mass_spec::nta::metfrag_screening
{
using Json = nlohmann::json;
    Json run_impl(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters, bool output_internal_standards)
    {
        // Tool resolution is discovery-only. Installation must be an explicit user action.
        const auto tool = ::streamfind::mass_spec::tools::resolve_metfrag();
        if (!tool)
            throw Error(ErrorCode::MethodExecution,
                        "MetFrag screening dependencies are unavailable. " +
                        ::streamfind::mass_spec::tools::tool_status() +
                        "Install Java 21 under <streamfind-home>/tools/java/jdk-*/bin/ and MetFragCL 2.6.11 as "
                        "<streamfind-home>/tools/metfrag/MetFragCL.jar, then retry. "
                        "MetFragCL download: https://github.com/ipb-halle/MetFragRelaunched/releases/download/v2.6.11/"
                        "MetFragCommandLine-2.6.11.jar");

        // R method defaults.
        const double ppm = parameters.value("ppm", 10.0);
        const double isotope_ppm = parameters.value("isotope_ppm", 5.0);
        const double sec = parameters.value("sec", 15.0);
        const double ppm_ms2 = parameters.value("ppm_ms2", 10.0);
        const double mzr_ms2 = parameters.value("mzr_ms2", 0.008);
        const int top_n = parameters.value("top_n", 5);
        std::vector<std::string> score_types;
        for (const auto &v : parameters.value("score_types", Json::array({Json("FragmenterScore")})))
            score_types.push_back(v.get<std::string>());
        std::vector<double> score_weights;
        for (const auto &v : parameters.value("score_weights", Json::array({Json(1.0)})))
            score_weights.push_back(v.get<double>());
        std::vector<std::string> pre_processing_candidate_filter;
        for (const auto &v : parameters.value("pre_processing_candidate_filter",
                                              Json::array({Json("UnconnectedCompoundFilter"),
                                                           Json("IsotopeFilter")})))
            pre_processing_candidate_filter.push_back(v.get<std::string>());
        std::vector<std::string> post_processing_candidate_filter;
        for (const auto &v : parameters.value("post_processing_candidate_filter", Json::array({Json("InChIKeyFilter")})))
            post_processing_candidate_filter.push_back(v.get<std::string>());
        const int maximum_tree_depth = parameters.value("maximum_tree_depth", 2);
        const int number_threads = parameters.value("number_threads", 1);
        const int feature_parallelism = parameters.value("feature_parallelism", 2);
        const bool cache_candidates = parameters.value("cache_candidates", true);
        const bool include_fragment_smiles = parameters.value("include_fragment_smiles", true);
        const bool use_smiles = parameters.value("use_smiles", true);
        const bool filtered = parameters.value("filtered", false);
        std::string database_type = output_internal_standards
            ? std::string("LocalCSV")
            : metfrag_runner::canonicalize_database_type(
                parameters.value("database_type", std::string("LocalCSV")));
        // `debug` is accepted for schema parity; the runner keeps the inspectable
        // PSV output for features with candidates regardless (R never forwards it).

        if (ppm < 0 || isotope_ppm < 0 || sec < 0 || ppm_ms2 < 0 || mzr_ms2 < 0)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: ppm, sec, ppm_ms2, and mzr_ms2 must be >= 0");
        if (top_n < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: top_n must be >= 1");
        if (maximum_tree_depth < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: maximum_tree_depth must be >= 1");
        if (number_threads < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: number_threads must be >= 1");
        if (feature_parallelism < 1)
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: feature_parallelism must be >= 1");
        if (score_types.size() != score_weights.size())
            throw Error(ErrorCode::InvalidArgument, "invalid metfrag_screening parameters: score_types and score_weights must have the same length");

        // Both MetFrag operations receive candidates through the
        // suspect_targets parameter. They intentionally do not consume a
        // connected suspectTargetsTable input.
        const auto suspect_targets = utils::detail::parse_suspect_targets(access, parameters, false);
        if (output_internal_standards && suspect_targets.empty())
            throw Error(ErrorCode::InvalidArgument,
                        "find_internal_standards_metfrag requires at least one suspect target row.");
        if (!suspect_targets.empty())
            database_type = "LocalCSV";

        const std::string database_path = parameters.value("database_path", std::string{});
        const bool external_local_database = suspect_targets.empty() &&
            (database_type == "LocalCSV" || database_type == "PubChemLite" || database_type == "CompToxCSV");
        if (!output_internal_standards && external_local_database && database_path.empty())
            throw Error(ErrorCode::InvalidArgument,
                        "database_type '" + database_type + "' requires an existing database_path when suspect targets are not supplied.");

        Json local_database = Json::array();
        if (external_local_database && !database_path.empty() &&
            !std::filesystem::exists(database_path))
            throw Error(ErrorCode::InvalidArgument,
                        "database_path does not exist for selected database_type '" + database_type + "': " + database_path);
        if (!output_internal_standards && suspect_targets.empty() &&
            !external_local_database &&
            !metfrag_runner::trim_ws(database_path).empty())
            throw Error(ErrorCode::InvalidArgument,
                        "database_path is only valid with LocalCSV, PubChemLite, or CompToxCSV; selected database_type is '" + database_type + "'.");
        if (external_local_database && !database_path.empty())
        {
            const std::string extension = metfrag_runner::to_lower_ascii(
                std::filesystem::path(database_path).extension().string());
            const bool supported_extension = extension == ".csv" || extension == ".psv" || extension == ".sdf";
            if (!supported_extension)
                throw Error(ErrorCode::InvalidArgument,
                            "database_path for selected database_type '" + database_type +
                            "' must have a .csv, .psv, or .sdf extension.");
            if ((database_type == "PubChemLite" || database_type == "CompToxCSV") && extension != ".csv")
                throw Error(ErrorCode::InvalidArgument,
                            "database_type '" + database_type + "' requires a .csv database_path.");
        }
        std::vector<SuspectQuery> normalized_targets;
        normalized_targets.reserve(suspect_targets.size());
        for (const auto &target : suspect_targets)
        {
            SuspectQuery normalized_target = target;
            std::string smiles = target.SMILES;
            std::string inchi = target.InChI;
            std::string inchikey = target.InChIKey;
            std::string formula = target.formula;
            double mass = target.has_mass ? target.mass : std::numeric_limits<double>::quiet_NaN();
            double xlogp = target.has_xLogP ? target.xLogP : std::numeric_limits<double>::quiet_NaN();
            const bool normalized = metfrag_runner::normalize_structure_fields(smiles, inchi, inchikey, formula, mass, xlogp);
            if (!target.has_mass && !normalized)
                throw Error(ErrorCode::InvalidArgument,
                            "metfrag_screening suspect_targets rows must provide mass or a valid SMILES/InChI.");

            normalized_target.SMILES = smiles;
            normalized_target.InChI = inchi;
            normalized_target.InChIKey = inchikey;
            normalized_target.formula = formula;
            normalized_target.mass = mass;
            normalized_target.has_mass = std::isfinite(mass);
            normalized_target.xLogP = xlogp;
            normalized_target.has_xLogP = std::isfinite(xlogp);
            normalized_targets.push_back(normalized_target);

            Json row = Json::object();
            row["name"] = target.name;

            row["formula"] = formula;
            row["mass"] = mass;
            row["SMILES"] = smiles;
            row["InChI"] = inchi;
            row["InChIKey"] = inchikey;
            row["xLogP"] = xlogp;
            local_database.push_back(std::move(row));
        }

        auto data = utils::detail::load_analysis_features(access, parameters);
        ::streamfind::mass_spec::nta::metfrag_runner::MetFragParams p;
        p.metfrag_path = tool->second; // MetFragCL.jar
        p.java_path = tool->first;     // java executable
        p.database_type = (database_type == "PubChemLite" || database_type == "CompToxCSV")
            ? "LocalCSV" : database_type;
        p.comp_tox_database = database_type == "CompToxCSV";
        p.feature_parallelism = feature_parallelism;
        p.cache_candidates = cache_candidates;
        p.ppm = ppm;
        p.isotopePpm = isotope_ppm;
        p.sec = sec;
        p.ppmMS2 = ppm_ms2;
        p.mzrMS2 = mzr_ms2;
        p.top_n = top_n;
        p.score_types = std::move(score_types);
        p.score_weights = std::move(score_weights);
        p.pre_processing_candidate_filter = std::move(pre_processing_candidate_filter);
        p.post_processing_candidate_filter = std::move(post_processing_candidate_filter);
        p.include_fragment_smiles = include_fragment_smiles;
        p.candidate_writer = include_fragment_smiles
            ? std::vector<std::string>{"CSV", "FragmentSmilesPSV"}
            : std::vector<std::string>{"CSV"};
        p.maximum_tree_depth = maximum_tree_depth;
        p.number_threads = number_threads;
        p.use_smiles = use_smiles;
        p.filtered = filtered;
        p.run_dir = ::streamfind::mass_spec::nta::metfrag_runner::resolve_run_dir(p);
        p.suspect_targets = std::move(normalized_targets);
        std::filesystem::create_directories(p.run_dir);
        if (database_type == "PubChemLite" || database_type == "CompToxCSV")
            p.database_path = database_path;
        else if (p.database_type == "LocalCSV")
            p.database_path = suspect_targets.empty()
                ? database_path
                : utils::detail::write_local_metfrag_database(local_database, p.run_dir);
        ::streamfind::mass_spec::nta::metfrag_runner::metfrag_screening_impl(access, data, p);
        if (output_internal_standards)
        {
            ::streamfind::mass_spec::nta::suspect_screening::convert_suspects_to_internal_standards(data);
            utils::detail::emit_internal_standards(access, data);
            return Json{{"status", "finished"}, {"info", "Internal standards found with MetFrag."}};
        }
        utils::detail::emit_suspects(access, data);
        return Json{{"status", "finished"}, {"info", "MetFrag screening completed."}};
    }

    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return run_impl(access, parameters, false);
    }
}

namespace streamfind::mass_spec::nta::find_internal_standards_metfrag
{
using Json = nlohmann::json;

    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        return ::streamfind::mass_spec::nta::metfrag_screening::run_impl(access, parameters, true);
    }
}
