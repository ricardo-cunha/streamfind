#include "streamfind/external/openbabel/streamfind_openbabel_api.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <vector>
#endif

#include <openbabel/descriptor.h>
#include <openbabel/elements.h>
#include <openbabel/groupcontrib.h>
#include <openbabel/mol.h>
#include <openbabel/obconversion.h>
#include <openbabel/oberror.h>
#include <openbabel/tokenst.h>

namespace streamfind::obabel_detail
{
#ifdef _WIN32
  std::wstring widen_path(const std::string &path)
  {
    return std::wstring(path.begin(), path.end());
  }

  std::string narrow_path(const std::wstring &path)
  {
    if (path.empty())
      return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
      return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), result.data(), length, nullptr, nullptr);
    return result;
  }

  std::wstring parent_directory(const std::wstring &path)
  {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
      return L"";
    return path.substr(0, pos);
  }

  bool directory_exists(const std::wstring &path)
  {
    if (path.empty())
      return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }

  std::wstring current_working_directory()
  {
    DWORD size = GetCurrentDirectoryW(0, nullptr);
    if (size == 0)
      return L"";
    std::vector<wchar_t> buffer(size, L'\0');
    DWORD written = GetCurrentDirectoryW(size, buffer.data());
    if (written == 0 || written >= size)
      return L"";
    return std::wstring(buffer.data(), written);
  }

  std::wstring current_module_path()
  {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&current_module_path),
            &self))
      return L"";

    std::vector<wchar_t> module_path(MAX_PATH, L'\0');
    DWORD path_size = GetModuleFileNameW(self, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (path_size == 0)
      return L"";
    if (path_size >= module_path.size())
    {
      module_path.resize(path_size + 1, L'\0');
      path_size = GetModuleFileNameW(self, module_path.data(), static_cast<DWORD>(module_path.size()));
      if (path_size == 0)
        return L"";
    }
    return std::wstring(module_path.data(), path_size);
  }

  bool ensure_openbabel_data_dir()
  {
    static bool configured = false;
    static std::once_flag configure_once;
    std::call_once(configure_once, []() {
      const std::wstring module_path = current_module_path();
      const std::wstring module_dir = parent_directory(module_path);
      const std::wstring parent1 = parent_directory(module_dir);
      const std::wstring parent2 = parent_directory(parent1);
      const std::wstring parent3 = parent_directory(parent2);
      const std::wstring parent4 = parent_directory(parent3);
      const std::wstring cwd = current_working_directory();
      const std::wstring source_file = widen_path(__FILE__);
      const std::wstring source_dir = parent_directory(source_file);
      const std::wstring source_root = parent_directory(source_dir);
      wchar_t env_buffer[32767];
      const DWORD env_size = GetEnvironmentVariableW(L"STREAMFIND_OPENBABEL_DATA", env_buffer, 32767);
      const std::wstring env_data = env_size > 0 ? std::wstring(env_buffer, env_size) : L"";

      const std::vector<std::wstring> candidates = {
          env_data,
          parent1.empty() ? L"" : parent1 + L"\\extdata\\openbabel\\data",
          parent1.empty() ? L"" : parent1 + L"\\core\\external\\openbabel\\openbabel-3-2-0\\data",
          source_root.empty() ? L"" : source_root + L"\\openbabel-3-2-0\\data",
          cwd.empty() ? L"" : cwd + L"\\src\\core\\external\\openbabel\\openbabel-3-2-0\\data"};

      for (const auto &candidate : candidates)
      {
        if (!directory_exists(candidate))
          continue;
        configured = SetEnvironmentVariableW(L"BABEL_DATADIR", candidate.c_str()) != 0;
        if (configured)
        {
          const std::string narrow_candidate = narrow_path(candidate);
          configured = _putenv_s("BABEL_DATADIR", narrow_candidate.c_str()) == 0;
        }
        if (configured)
          return;
      }
    });

    return configured;
  }

  std::string debug_openbabel_runtime()
  {
    const std::wstring module_path = current_module_path();
    const std::wstring module_dir = parent_directory(module_path);
    const std::wstring parent1 = parent_directory(module_dir);
    const std::wstring parent2 = parent_directory(parent1);
    const std::wstring parent3 = parent_directory(parent2);
    const std::wstring parent4 = parent_directory(parent3);
    const std::wstring cwd = current_working_directory();
    const std::wstring source_file = widen_path(__FILE__);
    const std::wstring source_dir = parent_directory(source_file);
    const std::wstring source_root = parent_directory(source_dir);
    const char *env = std::getenv("BABEL_DATADIR");
    const char *env_streamfind = std::getenv("STREAMFIND_OPENBABEL_DATA");

    const std::vector<std::wstring> candidates = {
        env_streamfind != nullptr ? widen_path(env_streamfind) : L"",
        parent1.empty() ? L"" : parent1 + L"\\extdata\\openbabel\\data",
        parent1.empty() ? L"" : parent1 + L"\\core\\external\\openbabel\\openbabel-3-2-0\\data",
        source_root.empty() ? L"" : source_root + L"\\openbabel-3-2-0\\data",
        cwd.empty() ? L"" : cwd + L"\\src\\core\\external\\openbabel\\openbabel-3-2-0\\data"};

    std::ostringstream oss;
    oss << "module_path=" << narrow_path(module_path) << "\n";
    oss << "cwd=" << narrow_path(cwd) << "\n";
    oss << "__FILE__=" << __FILE__ << "\n";
    oss << "env.STREAMFIND_OPENBABEL_DATA=" << (env_streamfind != nullptr ? env_streamfind : "<unset>") << "\n";
    oss << "env.BABEL_DATADIR=" << (env != nullptr ? env : "<unset>") << "\n";
    oss << "ensure_openbabel_data_dir=" << (ensure_openbabel_data_dir() ? "ok" : "failed") << "\n";
    env = std::getenv("BABEL_DATADIR");
    oss << "env.BABEL_DATADIR.after=" << (env != nullptr ? env : "<unset>") << "\n";
    for (size_t i = 0; i < candidates.size(); ++i)
    {
      oss << "candidate[" << i << "]=" << narrow_path(candidates[i])
          << " exists=" << (directory_exists(candidates[i]) ? "true" : "false") << "\n";
    }

    std::ifstream logp_stream;
    const std::string opened_logp = OpenBabel::OpenDatafile(logp_stream, "logp.txt");
    oss << "OpenDatafile(logp.txt)=" << (opened_logp.empty() ? "<empty>" : opened_logp) << "\n";
    oss << "OpenDatafile(logp.txt).good=" << (logp_stream.good() ? "true" : "false") << "\n";

    std::ifstream psa_stream;
    const std::string opened_psa = OpenBabel::OpenDatafile(psa_stream, "psa.txt");
    oss << "OpenDatafile(psa.txt)=" << (opened_psa.empty() ? "<empty>" : opened_psa) << "\n";
    oss << "OpenDatafile(psa.txt).good=" << (psa_stream.good() ? "true" : "false") << "\n";

    std::ifstream mr_stream;
    const std::string opened_mr = OpenBabel::OpenDatafile(mr_stream, "mr.txt");
    oss << "OpenDatafile(mr.txt)=" << (opened_mr.empty() ? "<empty>" : opened_mr) << "\n";
    oss << "OpenDatafile(mr.txt).good=" << (mr_stream.good() ? "true" : "false") << "\n";

    OpenBabel::OBDescriptor *logp_desc = OpenBabel::OBDescriptor::FindType("logP");
    oss << "OBDescriptor::FindType(logP)=" << (logp_desc != nullptr ? "found" : "null") << "\n";
    if (logp_desc != nullptr)
    {
      OpenBabel::OBMol test_mol;
      OpenBabel::OBConversion conv;
      const bool in_ok = conv.SetInFormat("smi");
      const bool read_ok = in_ok && conv.ReadString(&test_mol, "c1ccccc1");
      oss << "testmol.read_ok=" << (read_ok ? "true" : "false") << "\n";
      if (read_ok)
      {
        const double pred = logp_desc->Predict(&test_mol);
        oss << "logP.predict.benzene=" << pred << "\n";
      }
    }

    return oss.str();
  }
#endif

  std::string trim_copy(const std::string &value)
  {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
      return std::isspace(ch) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
      return std::isspace(ch) != 0;
    }).base();
    if (begin >= end)
      return "";
    return std::string(begin, end);
  }

  std::string normalize_formula(const std::string &formula)
  {
    std::string out;
    out.reserve(formula.size() + 8);
    for (size_t i = 0; i < formula.size(); ++i)
    {
      if (formula[i] == 'D')
      {
        out += "[2H]";
      }
      else if (formula[i] == 'T')
      {
        out += "[3H]";
      }
      else
      {
        out += formula[i];
      }
    }
    return out;
  }

  bool finite_value(double value)
  {
#ifdef _WIN32
    return _finite(value) != 0;
#else
    return std::isfinite(value);
#endif
  }

  bool read_molecule(OpenBabel::OBMol &mol,
                     const std::string &source,
                     const char *format_id,
                     std::string &error)
  {
    OpenBabel::OBConversion conv;
    if (!conv.SetInFormat(format_id))
    {
      error = std::string("Open Babel format unavailable: ") + format_id;
      return false;
    }
    if (!conv.ReadString(&mol, source))
    {
      error = std::string("Open Babel could not parse ") + format_id;
      return false;
    }
    return mol.NumAtoms() > 0;
  }

  std::string write_molecule(OpenBabel::OBMol &mol,
                             const char *format_id,
                             std::string &error)
  {
    OpenBabel::OBConversion conv;
    if (!conv.SetOutFormat(format_id))
    {
      error = std::string("Open Babel format unavailable: ") + format_id;
      return "";
    }
    const std::string result = trim_copy(conv.WriteString(&mol, true));
    if (result.empty())
    {
      error = std::string("Open Babel could not write ") + format_id;
    }
    return result;
  }

  void zero_result(streamfind_ob_normalized_result *out)
  {
    if (out != nullptr)
      std::memset(out, 0, sizeof(*out));
  }

  void zero_svg_result(streamfind_ob_svg_result *out)
  {
    if (out != nullptr)
      std::memset(out, 0, sizeof(*out));
  }

  void copy_text(char *dest, size_t capacity, const std::string &value)
  {
    if (dest == nullptr || capacity == 0)
      return;
    std::strncpy(dest, value.c_str(), capacity - 1);
    dest[capacity - 1] = '\0';
  }

  std::string normalize_svg_output(std::string svg, int width_px, int height_px)
  {
    if (width_px > 0)
    {
      const std::string width_attr = "width=\"";
      const size_t width_pos = svg.find(width_attr);
      if (width_pos != std::string::npos)
      {
        const size_t width_end = svg.find('"', width_pos + width_attr.size());
        if (width_end != std::string::npos)
        {
          svg.replace(width_pos + width_attr.size(), width_end - (width_pos + width_attr.size()), std::to_string(width_px) + "px");
        }
      }
    }
    if (height_px > 0)
    {
      const std::string height_attr = "height=\"";
      const size_t height_pos = svg.find(height_attr);
      if (height_pos != std::string::npos)
      {
        const size_t height_end = svg.find('"', height_pos + height_attr.size());
        if (height_end != std::string::npos)
        {
          svg.replace(height_pos + height_attr.size(), height_end - (height_pos + height_attr.size()), std::to_string(height_px) + "px");
        }
      }
    }

    const std::string rect_token = "<rect";
    const std::string fill_token = "fill=\"none\"";
    size_t search_pos = 0;
    while ((search_pos = svg.find(rect_token, search_pos)) != std::string::npos)
    {
      const size_t tag_end = svg.find('>', search_pos);
      if (tag_end == std::string::npos)
        break;
      const std::string tag = svg.substr(search_pos, tag_end - search_pos + 1);
      if (tag.find(fill_token) == std::string::npos &&
          tag.find("width=\"100%\"") != std::string::npos &&
          tag.find("height=\"100%\"") != std::string::npos)
      {
        svg.erase(search_pos, tag_end - search_pos + 1);
        continue;
      }
      search_pos = tag_end + 1;
    }
    return svg;
  }
}

// The legacy C-API shim mixed these file-local helpers with the exported
// extern "C" entry points that call them unqualified. Make them visible to
// the surrounding translation unit without an anonymous namespace.
using namespace streamfind::obabel_detail;

extern "C"
{
  int sf_ob_openbabel_available(void)
  {
#ifdef _WIN32
    ensure_openbabel_data_dir();
#endif
    return OpenBabel::OBConversion::FindFormat("smi") != nullptr &&
           OpenBabel::OBConversion::FindFormat("can") != nullptr &&
           OpenBabel::OBConversion::FindFormat("inchi") != nullptr &&
           OpenBabel::OBConversion::FindFormat("inchikey") != nullptr;
  }

  int sf_ob_normalize_structure(
    const char *smiles,
    const char *inchi,
    streamfind_ob_normalized_result *out)
  {
    zero_result(out);
    if (out == nullptr)
      return 0;

#ifdef _WIN32
    ensure_openbabel_data_dir();
#endif
    OpenBabel::obErrorLog.SetOutputLevel(OpenBabel::obError);

    OpenBabel::OBMol mol;
    std::string parse_error;
    bool parsed = false;

    if (smiles != nullptr && *smiles != '\0')
      parsed = read_molecule(mol, smiles, "smi", parse_error);
    if (!parsed && inchi != nullptr && *inchi != '\0')
    {
      mol.Clear();
      parsed = read_molecule(mol, inchi, "inchi", parse_error);
    }
    if (!parsed)
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY,
                parse_error.empty() ? std::string("No valid structure source available.") : parse_error);
      return 0;
    }

    std::string error;
    const std::string canonical_smiles = write_molecule(mol, "can", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    const std::string normalized_inchi = write_molecule(mol, "inchi", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    const std::string inchikey = write_molecule(mol, "inchikey", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    copy_text(out->canonical_smiles, streamfind_OB_SMILES_CAPACITY, canonical_smiles);
    copy_text(out->inchi, streamfind_OB_INCHI_CAPACITY, normalized_inchi);
    copy_text(out->inchikey, streamfind_OB_INCHIKEY_CAPACITY, inchikey);
    copy_text(out->formula, streamfind_OB_FORMULA_CAPACITY, normalize_formula(mol.GetFormula()));
    out->exact_mass = mol.GetExactMass();

    if (OpenBabel::OBDescriptor *logp = OpenBabel::OBDescriptor::FindType("logP"))
    {
      const double value = logp->Predict(&mol);
      if (finite_value(value))
      {
        out->xlogp = value;
        out->has_xlogp = 1;
      }
    }

    out->ok = 1;
    return 1;
  }

  int sf_ob_normalize_structure_from_mol_file(
    const char *file_path,
    streamfind_ob_normalized_result *out)
  {
    zero_result(out);
    if (out == nullptr || file_path == nullptr || *file_path == '\0')
      return 0;

#ifdef _WIN32
    ensure_openbabel_data_dir();
#endif
    OpenBabel::obErrorLog.SetOutputLevel(OpenBabel::obError);

    OpenBabel::OBMol mol;
    OpenBabel::OBConversion conv;
    if (!conv.SetInFormat("mol"))
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY,
                "Open Babel format unavailable: mol");
      return 0;
    }
    if (!conv.ReadFile(&mol, file_path))
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY,
                "Open Babel could not read file");
      return 0;
    }

    if (mol.NumAtoms() == 0)
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY,
                "No atoms in molecule");
      return 0;
    }

    std::string error;
    const std::string canonical_smiles = write_molecule(mol, "can", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    const std::string normalized_inchi = write_molecule(mol, "inchi", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    const std::string inchikey = write_molecule(mol, "inchikey", error);
    if (!error.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, error);
      return 0;
    }

    copy_text(out->canonical_smiles, streamfind_OB_SMILES_CAPACITY, canonical_smiles);
    copy_text(out->inchi, streamfind_OB_INCHI_CAPACITY, normalized_inchi);
    copy_text(out->inchikey, streamfind_OB_INCHIKEY_CAPACITY, inchikey);
    copy_text(out->formula, streamfind_OB_FORMULA_CAPACITY, normalize_formula(mol.GetFormula()));
    out->exact_mass = mol.GetExactMass();

    if (OpenBabel::OBDescriptor *logp = OpenBabel::OBDescriptor::FindType("logP"))
    {
      const double value = logp->Predict(&mol);
      if (finite_value(value))
      {
        out->xlogp = value;
        out->has_xlogp = 1;
      }
    }

    out->ok = 1;
    return 1;
  }

  namespace streamfind::obabel_formula_detail
  {
    struct FormulaElement
    {
      std::string symbol;
      double exact_mass;
      int min_count;
      int max_count;
    };

    void record_formula_dynamic(
        const std::vector<int> &counts,
        const std::vector<FormulaElement> &elements,
        double formula_mass,
        double error_da,
        double target_mass,
        streamfind_ob_formula_result *out)
    {
      if (out->count >= streamfind_OB_FORMULA_MAX_RESULTS)
        return;

      std::string formula;
      // Hill notation: C first, H second, then others alphabetically
      // Find indices for special ordering
      int idx_C = -1, idx_H = -1;
      for (int i = 0; i < static_cast<int>(elements.size()); ++i)
      {
        if (elements[i].symbol == "C") idx_C = i;
        if (elements[i].symbol == "H") idx_H = i;
      }

      if (idx_C >= 0 && counts[idx_C] > 0)
      {
        formula += "C";
        if (counts[idx_C] > 1) formula += std::to_string(counts[idx_C]);
      }
      if (idx_H >= 0 && counts[idx_H] > 0)
      {
        formula += "H";
        if (counts[idx_H] > 1) formula += std::to_string(counts[idx_H]);
      }
      for (int i = 0; i < static_cast<int>(elements.size()); ++i)
      {
        if (i == idx_C || i == idx_H) continue;
        if (counts[i] > 0)
        {
          formula += elements[i].symbol;
          if (counts[i] > 1) formula += std::to_string(counts[i]);
        }
      }

      if (formula.empty())
        return;

      const double error_ppm = (error_da / target_mass) * 1e6;

      std::strncpy(out->formulas[out->count], formula.c_str(),
                   streamfind_OB_FORMULA_STR_SIZE - 1);
      out->formulas[out->count][streamfind_OB_FORMULA_STR_SIZE - 1] = '\0';
      out->masses[out->count] = formula_mass;
      out->errors[out->count] = error_ppm;
      out->count++;
    }

    bool enumerate_formulas_recursive(
        double remaining_mass,
        double tolerance_da,
        int depth,
        const std::vector<FormulaElement> &elements,
        std::vector<int> &counts,
        double current_mass,
        double target_mass,
        streamfind_ob_formula_result *out)
    {
      if (out->count >= streamfind_OB_FORMULA_MAX_RESULTS)
        return false;

      if (depth == static_cast<int>(elements.size()))
      {
        const double error_da = std::fabs(remaining_mass);
        if (error_da <= tolerance_da)
        {
          record_formula_dynamic(counts, elements, current_mass,
                                 error_da, target_mass, out);
        }
        return true;
      }

      const double elem_mass = elements[depth].exact_mass;
      const int min_n = elements[depth].min_count;
      const int max_n = std::min(elements[depth].max_count,
          static_cast<int>((remaining_mass + tolerance_da) / elem_mass) + 1);

      // If this is the last element, compute n directly
      if (depth == static_cast<int>(elements.size()) - 1)
      {
        int n = static_cast<int>(std::round(remaining_mass / elem_mass));
        if (n >= min_n && n <= max_n)
        {
          counts[depth] = n;
          const double new_mass = current_mass + n * elem_mass;
          const double new_remaining = target_mass - new_mass;
          const double error_da = std::fabs(new_remaining);
          if (error_da <= tolerance_da)
          {
            record_formula_dynamic(counts, elements, new_mass,
                                   error_da, target_mass, out);
          }
        }
        return true;
      }

      for (int n = min_n; n <= max_n; ++n)
      {
        counts[depth] = n;
        const double new_current = current_mass + n * elem_mass;
        const double new_remaining = target_mass - new_current;
        if (new_remaining < -tolerance_da)
          break;
        if (!enumerate_formulas_recursive(
                new_remaining, tolerance_da,
                depth + 1, elements, counts,
                new_current, target_mass, out))
          return false;
      }
      return true;
    }

    int parse_atomic_number(const std::string &symbol)
    {
      if (symbol == "H")  return 1;
      if (symbol == "He") return 2;
      if (symbol == "Li") return 3;
      if (symbol == "Be") return 4;
      if (symbol == "B")  return 5;
      if (symbol == "C")  return 6;
      if (symbol == "N")  return 7;
      if (symbol == "O")  return 8;
      if (symbol == "F")  return 9;
      if (symbol == "Ne") return 10;
      if (symbol == "Na") return 11;
      if (symbol == "Mg") return 12;
      if (symbol == "Al") return 13;
      if (symbol == "Si") return 14;
      if (symbol == "P")  return 15;
      if (symbol == "S")  return 16;
      if (symbol == "Cl") return 17;
      if (symbol == "Ar") return 18;
      if (symbol == "K")  return 19;
      if (symbol == "Ca") return 20;
      if (symbol == "Br") return 35;
      if (symbol == "I")  return 53;
      return 0;
    }

    bool parse_element_constraint(
        const std::string &token,
        std::string &symbol,
        int &min_count,
        int &max_count)
    {
      // Format: "Symbol" or "Symbol:max" or "Symbol:min-max"
      const size_t colon = token.find(':');
      if (colon == std::string::npos)
      {
        symbol = token;
        min_count = 0;
        max_count = 999;
        return true;
      }

      symbol = token.substr(0, colon);
      const std::string range = token.substr(colon + 1);

      const size_t dash = range.find('-');
      if (dash == std::string::npos)
      {
        // "Symbol:max" format
        min_count = 0;
        max_count = std::stoi(range);
      }
      else
      {
        // "Symbol:min-max" format
        min_count = std::stoi(range.substr(0, dash));
        max_count = std::stoi(range.substr(dash + 1));
      }
      return true;
    }

    void sort_formula_results(streamfind_ob_formula_result *out)
    {
      if (out->count <= 1)
        return;
      for (int i = 1; i < out->count; ++i)
      {
        for (int j = i; j > 0 && out->errors[j] < out->errors[j - 1]; --j)
        {
          std::swap(out->errors[j], out->errors[j - 1]);
          std::swap(out->masses[j], out->masses[j - 1]);
          char tmp[streamfind_OB_FORMULA_STR_SIZE];
          std::strncpy(tmp, out->formulas[j], streamfind_OB_FORMULA_STR_SIZE);
          std::strncpy(out->formulas[j], out->formulas[j - 1], streamfind_OB_FORMULA_STR_SIZE);
          std::strncpy(out->formulas[j - 1], tmp, streamfind_OB_FORMULA_STR_SIZE);
        }
      }
    }
  }

  using namespace streamfind::obabel_formula_detail;

  int sf_ob_formula_from_mass(
    double monoisotopic_mass,
    double tolerance_ppm,
    const char *elements,
    streamfind_ob_formula_result *out)
  {
    std::memset(out, 0, sizeof(*out));
    if (out == nullptr || monoisotopic_mass <= 0.0)
      return 0;

#ifdef _WIN32
    ensure_openbabel_data_dir();
#endif

    const double tolerance_da = monoisotopic_mass * tolerance_ppm * 1e-6;

    // Parse element constraints
    std::vector<FormulaElement> elems;
    int h_index = -1;

    if (elements != nullptr && *elements != '\0')
    {
      std::string elem_str(elements);
      std::istringstream stream(elem_str);
      std::string token;
      while (std::getline(stream, token, ';'))
      {
        if (token.empty()) continue;
        FormulaElement elem;
        if (!parse_element_constraint(token, elem.symbol,
                                       elem.min_count, elem.max_count))
          continue;
        const int an = parse_atomic_number(elem.symbol);
        if (an == 0) continue;
        elem.exact_mass = OpenBabel::OBElements::GetExactMass(an);
        if (elem.symbol == "H") h_index = static_cast<int>(elems.size());
        elems.push_back(elem);
      }
    }

    if (elems.empty())
    {
      FormulaElement c, h, n, o, p, s;
      c.symbol = "C"; c.exact_mass = OpenBabel::OBElements::GetExactMass(6);
      c.min_count = 0; c.max_count = std::min(130, static_cast<int>(monoisotopic_mass / c.exact_mass) + 1);
      h.symbol = "H"; h.exact_mass = OpenBabel::OBElements::GetExactMass(1);
      h.min_count = 0; h.max_count = std::min(260, static_cast<int>(monoisotopic_mass / h.exact_mass) + 1);
      h_index = 1;
      n.symbol = "N"; n.exact_mass = OpenBabel::OBElements::GetExactMass(7);
      n.min_count = 0; n.max_count = std::min(30, static_cast<int>(monoisotopic_mass / n.exact_mass) + 1);
      o.symbol = "O"; o.exact_mass = OpenBabel::OBElements::GetExactMass(8);
      o.min_count = 0; o.max_count = std::min(30, static_cast<int>(monoisotopic_mass / o.exact_mass) + 1);
      p.symbol = "P"; p.exact_mass = OpenBabel::OBElements::GetExactMass(15);
      p.min_count = 0; p.max_count = std::min(5, static_cast<int>(monoisotopic_mass / p.exact_mass) + 1);
      s.symbol = "S"; s.exact_mass = OpenBabel::OBElements::GetExactMass(16);
      s.min_count = 0; s.max_count = std::min(10, static_cast<int>(monoisotopic_mass / s.exact_mass) + 1);
      elems = {c, h, n, o, p, s};
    }
    else
    {
      // Fill in auto bounds for any element that has max_count == 999
      for (auto &elem : elems)
      {
        if (elem.max_count == 999)
        {
          elem.max_count = static_cast<int>(monoisotopic_mass / elem.exact_mass) + 1;
        }
      }
    }

    // Ensure H is the last element for direct calculation optimization
    if (h_index >= 0 && h_index != static_cast<int>(elems.size()) - 1)
    {
      std::swap(elems[h_index], elems.back());
    }

    // Sort by descending mass (excluding H which is last) for efficient pruning
    if (elems.size() > 2)
    {
      const int sort_end = static_cast<int>(elems.size()) - 1;
      for (int i = 0; i < sort_end - 1; ++i)
      {
        for (int j = i + 1; j < sort_end; ++j)
        {
          if (elems[j].exact_mass > elems[i].exact_mass)
          {
            std::swap(elems[i], elems[j]);
          }
        }
      }
    }

    std::vector<int> counts(elems.size(), 0);
    enumerate_formulas_recursive(
        monoisotopic_mass, tolerance_da,
        0, elems, counts, 0.0, monoisotopic_mass, out);

    sort_formula_results(out);

    return out->count > 0 ? 1 : 0;
  }

  int sf_ob_render_structure_svg(
    const char *smiles,
    const char *inchi,
    int width_px,
    int height_px,
    const char *bond_color,
    streamfind_ob_svg_result *out)
  {
    zero_svg_result(out);
    if (out == nullptr)
      return 0;

#ifdef _WIN32
    ensure_openbabel_data_dir();
#endif
    OpenBabel::obErrorLog.SetOutputLevel(OpenBabel::obError);

    OpenBabel::OBMol mol;
    std::string parse_error;
    bool parsed = false;

    if (smiles != nullptr && *smiles != '\0')
      parsed = read_molecule(mol, smiles, "smi", parse_error);
    if (!parsed && inchi != nullptr && *inchi != '\0')
    {
      mol.Clear();
      parsed = read_molecule(mol, inchi, "inchi", parse_error);
    }
    if (!parsed)
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY,
                parse_error.empty() ? std::string("No valid structure source available.") : parse_error);
      return 0;
    }

    OpenBabel::OBConversion conv;
    if (!conv.SetOutFormat("svg"))
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, "Open Babel format unavailable: svg");
      return 0;
    }

    conv.AddOption("b", OpenBabel::OBConversion::OUTOPTIONS, "none");
    if (bond_color != nullptr && *bond_color != '\0')
      conv.AddOption("B", OpenBabel::OBConversion::OUTOPTIONS, bond_color);
    std::string error;
    std::string svg = trim_copy(conv.WriteString(&mol, true));
    if (svg.empty())
    {
      copy_text(out->error, streamfind_OB_ERROR_CAPACITY, "Open Babel could not write svg");
      return 0;
    }

    svg = normalize_svg_output(svg, width_px, height_px);
    copy_text(out->svg, streamfind_OB_SVG_CAPACITY, svg);
    out->ok = 1;
    return 1;
  }

  int sf_ob_debug_runtime(
    char *out,
    size_t capacity)
  {
    if (out == nullptr || capacity == 0)
      return 0;
#ifdef _WIN32
    const std::string text = debug_openbabel_runtime();
#else
    const std::string text = "Open Babel runtime debug is only implemented on Windows.";
#endif
    std::strncpy(out, text.c_str(), capacity - 1);
    out[capacity - 1] = '\0';
    return 1;
  }
}
