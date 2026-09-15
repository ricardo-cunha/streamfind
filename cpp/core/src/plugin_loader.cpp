#include "streamfind/plugin_loader.hpp"

#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace streamfind {

DynamicLibrary::DynamicLibrary(void *handle, std::filesystem::path path)
    : handle_(handle), path_(std::move(path)) {}

DynamicLibrary::DynamicLibrary(DynamicLibrary &&other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = nullptr;
}

DynamicLibrary &DynamicLibrary::operator=(DynamicLibrary &&other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = nullptr;
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() { reset(); }

DynamicLibrary::LoadResult DynamicLibrary::load(const std::filesystem::path &path) {
    LoadResult result;
    if (path.empty()) {
        result.diagnostics = "plugin library path is empty";
        return result;
    }

#if defined(_WIN32)
    const auto native_path = path.wstring();
    HMODULE module = LoadLibraryW(native_path.c_str());
    if (module == nullptr) {
        result.diagnostics = "LoadLibraryW failed for " + path.string();
        return result;
    }
    result.library = std::unique_ptr<DynamicLibrary>(
        new DynamicLibrary(reinterpret_cast<void *>(module), path));
#else
    void *module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char *error = dlerror();
        result.diagnostics = "dlopen failed for " + path.string();
        if (error != nullptr) {
            result.diagnostics += ": ";
            result.diagnostics += error;
        }
        return result;
    }
    result.library = std::unique_ptr<DynamicLibrary>(new DynamicLibrary(module, path));
#endif
    result.loaded = true;
    return result;
}

void *DynamicLibrary::symbol(const char *name, std::string &diagnostics) const {
    if (handle_ == nullptr || name == nullptr || *name == '\0') {
        diagnostics = "cannot resolve symbol from an invalid library or empty name";
        return nullptr;
    }
#if defined(_WIN32)
    FARPROC address = GetProcAddress(reinterpret_cast<HMODULE>(handle_), name);
    if (address == nullptr) {
        diagnostics = "GetProcAddress failed for symbol " + std::string(name);
        return nullptr;
    }
    return reinterpret_cast<void *>(address);
#else
    dlerror();
    void *address = dlsym(handle_, name);
    const char *error = dlerror();
    if (error != nullptr) {
        diagnostics = "dlsym failed for symbol " + std::string(name) + ": " + error;
        return nullptr;
    }
    return address;
#endif
}

void DynamicLibrary::reset() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

}  // namespace streamfind
