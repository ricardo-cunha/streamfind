#include "streamfind/external/openbabel_adapter.hpp"

#include "streamfind/external/openbabel/streamfind_openbabel_api.h"

#include <string>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <vector>
#endif

namespace sf::obabel
{
#ifdef _WIN32
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
#endif

  NormalizedStructure from_c_result(
      const streamfind_ob_normalized_result &result)
  {
    NormalizedStructure out;
    out.ok = result.ok != 0;
    out.error = result.error;
    out.canonical_smiles = result.canonical_smiles;
    out.formula = result.formula;
    out.inchi = result.inchi;
    out.inchikey = result.inchikey;
    out.exact_mass = result.exact_mass;
    out.xlogp = result.xlogp;
    out.has_xlogp = result.has_xlogp != 0;
    return out;
  }

  StructureSvg from_svg_result(
      const streamfind_ob_svg_result &result)
  {
    StructureSvg out;
    out.ok = result.ok != 0;
    out.error = result.error;
    out.svg = result.svg;
    return out;
  }

#ifdef _WIN32
  using openbabel_available_fn = int (*)();
  using normalize_structure_fn = int (*)(const char *, const char *, streamfind_ob_normalized_result *);
  using normalize_structure_from_mol_file_fn = int (*)(const char *, streamfind_ob_normalized_result *);
  using formula_from_mass_fn = int (*)(double, double, const char *, streamfind_ob_formula_result *);
  using render_structure_svg_fn = int (*)(const char *, const char *, int, int, const char *, streamfind_ob_svg_result *);
  using debug_runtime_fn = int (*)(char *, size_t);

  struct OpenBabelApi
  {
    HMODULE module = nullptr;
    openbabel_available_fn available = nullptr;
    normalize_structure_fn normalize = nullptr;
    normalize_structure_from_mol_file_fn normalize_from_mol = nullptr;
    formula_from_mass_fn formula_from_mass = nullptr;
    render_structure_svg_fn render_svg = nullptr;
    debug_runtime_fn debug_runtime = nullptr;
    std::string error;
  };

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

  std::wstring find_openbabel_data_dir(
      const std::wstring &dll_path,
      const std::wstring &module_dir,
      const std::wstring &cwd)
  {
    const std::wstring dll_dir = parent_directory(dll_path);
    const std::wstring dll_dir_parent = parent_directory(dll_dir);
    const std::wstring dll_dir_grandparent = parent_directory(dll_dir_parent);
    const std::wstring dll_dir_root = parent_directory(dll_dir_grandparent);
    const std::wstring dll_dir_top = parent_directory(dll_dir_root);

    wchar_t env_buffer[32767];
    const DWORD env_size = GetEnvironmentVariableW(L"STREAMFIND_OPENBABEL_DATA", env_buffer, 32767);
    const std::wstring env_data = env_size > 0 ? std::wstring(env_buffer, env_size) : L"";
    const std::wstring module_parent = parent_directory(module_dir);

    const std::vector<std::wstring> candidates = {
        env_data,
        dll_dir + L"\\openbabel\\data",
        module_parent.empty() ? L"" : module_parent + L"\\extdata\\openbabel\\data",
        module_parent.empty() ? L"" : module_parent + L"\\core\\external\\openbabel\\openbabel-3-2-0\\data",
        cwd.empty() ? L"" : cwd + L"\\src\\core\\external\\openbabel\\openbabel-3-2-0\\data"};

    for (const auto &candidate : candidates)
    {
      if (directory_exists(candidate))
        return candidate;
    }

    return L"";
  }

  bool configure_openbabel_data_dir(
      const std::wstring &dll_path,
      const std::wstring &module_dir,
      const std::wstring &cwd,
      std::string &error)
  {
    const std::wstring data_dir = find_openbabel_data_dir(dll_path, module_dir, cwd);
    if (data_dir.empty())
    {
      error = "Could not locate Open Babel data directory relative to " +
              narrow_path(dll_path);
      return false;
    }

    if (!SetEnvironmentVariableW(L"BABEL_DATADIR", data_dir.c_str()))
    {
      error = "Could not set BABEL_DATADIR for Open Babel runtime.";
      return false;
    }

    return true;
  }

  OpenBabelApi load_openbabel_api()
  {
    OpenBabelApi api;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&load_openbabel_api),
            &self))
    {
      api.error = "GetModuleHandleExW failed for streamfind module.";
      return api;
    }

    std::vector<wchar_t> module_path(MAX_PATH, L'\0');
    DWORD path_size = GetModuleFileNameW(self, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (path_size == 0)
    {
      api.error = "GetModuleFileNameW failed for streamfind module.";
      return api;
    }
    if (path_size >= module_path.size())
    {
      module_path.resize(path_size + 1, L'\0');
      path_size = GetModuleFileNameW(self, module_path.data(), static_cast<DWORD>(module_path.size()));
      if (path_size == 0)
      {
        api.error = "GetModuleFileNameW failed for streamfind module.";
        return api;
      }
    }

    const std::wstring module_dir = parent_directory(std::wstring(module_path.data(), path_size));
    const std::wstring cwd = current_working_directory();
    const std::wstring module_parent = parent_directory(module_dir);
    const std::vector<std::wstring> dll_candidates = {
        module_dir + L"\\openbabel_streamfind.dll",
        module_parent.empty() ? L"" : module_parent + L"\\openbabel_streamfind.dll",
        module_dir + L"\\core\\external\\openbabel\\build\\windows\\bin\\openbabel_streamfind.dll",
        cwd.empty() ? L"" : cwd + L"\\inst\\libs\\openbabel_streamfind.dll",
        cwd.empty() ? L"" : cwd + L"\\src\\core\\external\\openbabel\\build\\windows\\bin\\openbabel_streamfind.dll"};

    std::wstring loaded_path;
    for (const auto &dll_path : dll_candidates)
    {
      if (dll_path.empty())
        continue;
      api.module = LoadLibraryW(dll_path.c_str());
      if (api.module != nullptr)
      {
        loaded_path = dll_path;
        break;
      }
    }

    if (api.module == nullptr)
    {
      api.error = "Could not load openbabel_streamfind.dll from expected locations near " +
                  narrow_path(module_dir);
      return api;
    }

    if (!configure_openbabel_data_dir(loaded_path, module_dir, cwd, api.error))
    {
      FreeLibrary(api.module);
      api.module = nullptr;
      return api;
    }

    api.available = reinterpret_cast<openbabel_available_fn>(
        GetProcAddress(api.module, "sf_ob_openbabel_available"));
    api.normalize = reinterpret_cast<normalize_structure_fn>(
        GetProcAddress(api.module, "sf_ob_normalize_structure"));
    api.normalize_from_mol = reinterpret_cast<normalize_structure_from_mol_file_fn>(
        GetProcAddress(api.module, "sf_ob_normalize_structure_from_mol_file"));
    api.formula_from_mass = reinterpret_cast<formula_from_mass_fn>(
        GetProcAddress(api.module, "sf_ob_formula_from_mass"));
    api.render_svg = reinterpret_cast<render_structure_svg_fn>(
        GetProcAddress(api.module, "sf_ob_render_structure_svg"));
    api.debug_runtime = reinterpret_cast<debug_runtime_fn>(
        GetProcAddress(api.module, "sf_ob_debug_runtime"));

    if (api.available == nullptr || api.normalize == nullptr || api.render_svg == nullptr || api.debug_runtime == nullptr)
    {
      api.error = "Could not resolve Open Babel streamfind API exports from " +
                  narrow_path(loaded_path);
      FreeLibrary(api.module);
      api.module = nullptr;
      api.available = nullptr;
      api.normalize = nullptr;
      api.render_svg = nullptr;
      api.debug_runtime = nullptr;
    }

    return api;
  }
#endif

  bool openbabel_available()
  {
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    return api.available != nullptr && api.available() != 0;
#else
    return sf_ob_openbabel_available() != 0;
#endif
  }

  NormalizedStructure normalize_structure(
      const std::string &smiles,
      const std::string &inchi)
  {
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    if (api.normalize == nullptr)
    {
      NormalizedStructure out;
      out.error = api.error.empty() ? "Open Babel runtime unavailable." : api.error;
      return out;
    }

    streamfind_ob_normalized_result result{};
    api.normalize(
        smiles.empty() ? nullptr : smiles.c_str(),
        inchi.empty() ? nullptr : inchi.c_str(),
        &result);
    return from_c_result(result);
#else
    streamfind_ob_normalized_result result{};
    sf_ob_normalize_structure(
        smiles.empty() ? nullptr : smiles.c_str(),
        inchi.empty() ? nullptr : inchi.c_str(),
        &result);
    return from_c_result(result);
#endif
  }

  NormalizedStructure normalize_structure_from_mol_file(
      const std::string &file_path)
  {
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    if (api.normalize_from_mol == nullptr)
    {
      NormalizedStructure out;
      out.error = api.error.empty() ? "Open Babel runtime unavailable." : api.error;
      return out;
    }

    streamfind_ob_normalized_result result{};
    api.normalize_from_mol(
        file_path.empty() ? nullptr : file_path.c_str(),
        &result);
    return from_c_result(result);
#else
    streamfind_ob_normalized_result result{};
    sf_ob_normalize_structure_from_mol_file(
        file_path.empty() ? nullptr : file_path.c_str(),
        &result);
    return from_c_result(result);
#endif
  }

  std::vector<FormulaMatch> formula_from_mass(
      double monoisotopic_mass,
      double tolerance_ppm,
      const std::string &elements)
  {
    std::vector<FormulaMatch> out;
    const char *elem_cstr = elements.empty() ? nullptr : elements.c_str();
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    if (api.formula_from_mass == nullptr)
      return out;

    streamfind_ob_formula_result result{};
    api.formula_from_mass(monoisotopic_mass, tolerance_ppm, elem_cstr, &result);
    if (result.count > 0)
    {
      out.reserve(static_cast<size_t>(result.count));
      for (int i = 0; i < result.count; ++i)
      {
        FormulaMatch match;
        match.formula = result.formulas[i];
        match.exact_mass = result.masses[i];
        match.error_ppm = result.errors[i];
        out.push_back(std::move(match));
      }
    }
#else
    streamfind_ob_formula_result result{};
    sf_ob_formula_from_mass(monoisotopic_mass, tolerance_ppm, elem_cstr, &result);
    if (result.count > 0)
    {
      out.reserve(static_cast<size_t>(result.count));
      for (int i = 0; i < result.count; ++i)
      {
        FormulaMatch match;
        match.formula = result.formulas[i];
        match.exact_mass = result.masses[i];
        match.error_ppm = result.errors[i];
        out.push_back(std::move(match));
      }
    }
#endif
    return out;
  }

  StructureSvg render_structure_svg(
      const std::string &smiles,
      const std::string &inchi,
      int width_px,
      int height_px,
      const std::string &bond_color)
  {
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    if (api.render_svg == nullptr)
    {
      StructureSvg out;
      out.error = api.error.empty() ? "Open Babel runtime unavailable." : api.error;
      return out;
    }

    streamfind_ob_svg_result result{};
    api.render_svg(
        smiles.empty() ? nullptr : smiles.c_str(),
        inchi.empty() ? nullptr : inchi.c_str(),
        width_px,
        height_px,
        bond_color.empty() ? nullptr : bond_color.c_str(),
        &result);
    return from_svg_result(result);
#else
    streamfind_ob_svg_result result{};
    sf_ob_render_structure_svg(
        smiles.empty() ? nullptr : smiles.c_str(),
        inchi.empty() ? nullptr : inchi.c_str(),
        width_px,
        height_px,
        bond_color.empty() ? nullptr : bond_color.c_str(),
        &result);
    return from_svg_result(result);
#endif
  }

  std::string debug_runtime()
  {
#ifdef _WIN32
    const OpenBabelApi api = load_openbabel_api();
    if (api.debug_runtime == nullptr)
      return api.error.empty() ? "Open Babel runtime unavailable." : api.error;

    std::vector<char> buffer(streamfind_OB_DEBUG_CAPACITY, '\0');
    api.debug_runtime(buffer.data(), buffer.size());
    return std::string(buffer.data());
#else
    std::vector<char> buffer(streamfind_OB_DEBUG_CAPACITY, '\0');
    sf_ob_debug_runtime(buffer.data(), buffer.size());
    return std::string(buffer.data());
#endif
  }
} // namespace sf::obabel
