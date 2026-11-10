#include "operations/fragmentation/fragment_suspect_targets.hpp"
#include "operations/fragmentation/fragmenter_limits.hpp"
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace streamfind::mass_spec::fragmentation::detail
{
struct ProcessResult
{
    int exit_code = -1;
    std::string output;
    std::string error;
    bool timed_out = false;
    bool cancelled = false;
    bool output_limit_exceeded = false;
};

using StopPredicate = std::function<bool()>;
constexpr std::uint64_t kDiagnosticLimitBytes = 4096;

std::uint64_t output_size(const fs::path &stdout_path, const fs::path &stderr_path)
{
    std::error_code error;
    const auto stdout_size = fs::file_size(stdout_path, error);
    if (error) return std::numeric_limits<std::uint64_t>::max();
    const auto stderr_size = fs::file_size(stderr_path, error);
    if (error || stdout_size > std::numeric_limits<std::uint64_t>::max() - stderr_size)
        return std::numeric_limits<std::uint64_t>::max();
    return stdout_size + stderr_size;
}

std::string read_file_prefix(const fs::path &path, std::uint64_t maximum_bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input || maximum_bytes == 0) return {};
    std::error_code error;
    const auto file_bytes = fs::file_size(path, error);
    if (error) throw std::runtime_error("Could not inspect MetFrag Fragmenter output file size.");
    const auto bytes_to_read = std::min(maximum_bytes, file_bytes);
    std::string content;
    content.resize(static_cast<std::size_t>(bytes_to_read));
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(input.gcount()));
    return content;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
        const auto process_id = GetCurrentProcessId();
#else
        const auto process_id = getpid();
#endif
        path_ = fs::temp_directory_path() /
            ("streamfind-metfrag-fragmenter-" + std::to_string(process_id) + "-" + std::to_string(stamp));
        fs::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const fs::path &path() const { return path_; }
private:
    fs::path path_;
};

#ifdef _WIN32
std::wstring narrow_to_wide(const std::string &value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Could not convert a process argument to UTF-16.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring quote_windows_argument(const std::wstring &value)
{
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"')
        {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted += character;
        }
        else
        {
            quoted.append(slashes, L'\\');
            quoted += character;
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'\"';
    return quoted;
}

ProcessResult run_cli(const std::string &java, const std::string &jar,
                      const std::string &smiles, int depth,
                      std::uint32_t timeout_seconds, std::uint64_t output_limit_bytes,
                      const fs::path &stdout_path, const fs::path &stderr_path,
                      const StopPredicate &stop_requested)
{
    std::vector<std::wstring> arguments = {
        narrow_to_wide(java), L"-jar", narrow_to_wide(jar), L"--smiles", narrow_to_wide(smiles),
        L"--depth", std::to_wstring(depth)};
    std::wstring command;
    for (const auto &argument : arguments)
    {
        if (!command.empty()) command += L' ';
        command += quote_windows_argument(argument);
    }
    SECURITY_ATTRIBUTES security_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security_attributes,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE error = CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security_attributes,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE)
    {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
        throw std::runtime_error("Could not create temporary MetFrag Fragmenter output files.");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = error;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(output);
    CloseHandle(error);
    if (!created) throw std::runtime_error("Could not start Java for MetFrag Fragmenter (Windows error " +
                                            std::to_string(GetLastError()) + ").");
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool terminated = false;
    while (true)
    {
        const DWORD wait_result = WaitForSingleObject(process.hProcess, 100);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED)
        {
            TerminateProcess(process.hProcess, 1);
            terminated = true;
            break;
        }
        if (stop_requested && stop_requested())
        {
            TerminateProcess(process.hProcess, 1);
            result.cancelled = true;
            terminated = true;
            break;
        }
        if (output_size(stdout_path, stderr_path) > output_limit_bytes)
        {
            TerminateProcess(process.hProcess, 1);
            result.output_limit_exceeded = true;
            terminated = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(process.hProcess, 1);
            result.timed_out = true;
            terminated = true;
            break;
        }
    }
    if (terminated) WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit_code);
    if (output_size(stdout_path, stderr_path) > output_limit_bytes)
        result.output_limit_exceeded = true;
    if (!result.output_limit_exceeded)
        result.output = read_file_prefix(stdout_path, output_limit_bytes);
    result.error = read_file_prefix(stderr_path, kDiagnosticLimitBytes);
    return result;
}
#else
ProcessResult run_cli(const std::string &java, const std::string &jar,
                      const std::string &smiles, int depth,
                      std::uint32_t timeout_seconds, std::uint64_t output_limit_bytes,
                      const fs::path &stdout_path, const fs::path &stderr_path,
                      const StopPredicate &stop_requested)
{
    std::vector<std::string> arguments = {java, "-jar", jar, "--smiles", smiles, "--depth", std::to_string(depth)};
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const int output = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int error = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0 || error < 0)
    {
        if (output >= 0) close(output);
        if (error >= 0) close(error);
        throw std::runtime_error("Could not create temporary MetFrag Fragmenter output files.");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output);
    posix_spawn_file_actions_addclose(&actions, error);
    pid_t process = 0;
    const int spawned = posix_spawn(&process, java.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output);
    close(error);
    if (spawned != 0) throw std::runtime_error("Could not start Java for MetFrag Fragmenter (error " +
                                                std::to_string(spawned) + ").");
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    int status = 0;
    bool finished = false;
    while (!finished)
    {
        const pid_t waited = waitpid(process, &status, WNOHANG);
        if (waited == process)
        {
            finished = true;
            break;
        }
        if (waited < 0) throw std::runtime_error("Could not wait for MetFrag Fragmenter process.");
        if (stop_requested && stop_requested())
        {
            kill(process, SIGTERM);
            result.cancelled = true;
            const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < grace_deadline)
            {
                const pid_t grace_wait = waitpid(process, &status, WNOHANG);
                if (grace_wait == process) { finished = true; break; }
                if (grace_wait < 0) throw std::runtime_error("Could not wait for cancelled MetFrag Fragmenter process.");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!finished)
            {
                kill(process, SIGKILL);
                if (waitpid(process, &status, 0) < 0) throw std::runtime_error("Could not reap cancelled MetFrag Fragmenter process.");
                finished = true;
            }
            break;
        }
        if (output_size(stdout_path, stderr_path) > output_limit_bytes)
        {
            kill(process, SIGTERM);
            result.output_limit_exceeded = true;
            const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < grace_deadline)
            {
                const pid_t grace_wait = waitpid(process, &status, WNOHANG);
                if (grace_wait == process) { finished = true; break; }
                if (grace_wait < 0) throw std::runtime_error("Could not wait for output-limited MetFrag Fragmenter process.");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!finished)
            {
                kill(process, SIGKILL);
                if (waitpid(process, &status, 0) < 0) throw std::runtime_error("Could not reap output-limited MetFrag Fragmenter process.");
                finished = true;
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            kill(process, SIGTERM);
            result.timed_out = true;
            const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < grace_deadline)
            {
                const pid_t grace_wait = waitpid(process, &status, WNOHANG);
                if (grace_wait == process) { finished = true; break; }
                if (grace_wait < 0) throw std::runtime_error("Could not wait for timed-out MetFrag Fragmenter process.");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!finished)
            {
                kill(process, SIGKILL);
                if (waitpid(process, &status, 0) < 0) throw std::runtime_error("Could not reap timed-out MetFrag Fragmenter process.");
                finished = true;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (output_size(stdout_path, stderr_path) > output_limit_bytes)
        result.output_limit_exceeded = true;
    if (!result.output_limit_exceeded)
        result.output = read_file_prefix(stdout_path, output_limit_bytes);
    result.error = read_file_prefix(stderr_path, kDiagnosticLimitBytes);
    return result;
}
#endif

std::string json_scalar_text(const Json &value)
{
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

std::vector<std::string> split_connected_smiles(const std::string &smiles)
{
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= smiles.size())
    {
        const auto separator = smiles.find('.', start);
        const auto component = smiles.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
        if (!component.empty()) components.push_back(component);
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return components;
}

} // namespace streamfind::mass_spec::fragmentation::detail

namespace streamfind::mass_spec::fragmentation::fragment_suspect_targets
{
Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto table_input = inputs.find("suspectTargetsTable");
    if (table_input == inputs.end() || !table_input->is_object() ||
        !table_input->contains("physical_table") || !table_input->at("physical_table").is_string())
        throw std::invalid_argument("Connect a suspectTargetsTable input containing candidate SMILES.");

    const auto limits = streamfind::mass_spec::fragmentation::detail::parse_fragmenter_limits(parameters);
    const auto java = tools::resolve_java();
    if (!java) throw std::runtime_error("Java 21 was not found. Install the runtime.java dependency first.");
    const auto jar = tools::resolve_metfrag_fragmenter_jar();
    if (!jar)
        throw std::runtime_error("MetFrag Fragmenter JAR was not found. Install the mass_spec.metfrag_fragmenter dependency.");

    const auto physical_table = table_input->at("physical_table").get<std::string>();
    const auto targets = access.read(physical_table, {"name", "SMILES"}, "name");
    if (targets.empty()) throw std::invalid_argument("suspectTargetsTable contains no target rows.");

    const std::vector<std::string> columns = {
        "name", "fragment_id", "parent_id", "SMILES",
        "formula", "mass", "depth", "atom_indices", "broken_bond_indices", "neutral_losses", "fragment_data"};
    const std::vector<std::string> types = {
        "string", "string", "string", "string", "string", "real", "integer",
        "array", "array", "array", "object"};
    Json rows = Json::array();
    Json errors = Json::array();
    std::size_t skipped = 0;
    std::size_t fragmented = 0;
    std::size_t fragmented_components = 0;
    std::size_t total_components = 0;
    std::size_t failed_components = 0;
    for (const auto &target : targets)
    {
        const auto smiles_value = target.find("SMILES");
        const std::string smiles = smiles_value == target.end() || smiles_value->is_null()
            ? std::string{} : smiles_value->get<std::string>();
        if (smiles.empty()) { ++skipped; continue; }
        const auto name_value = target.find("name");
        const std::string name = name_value == target.end() || name_value->is_null() ? std::string{} : name_value->get<std::string>();
        const auto components = streamfind::mass_spec::fragmentation::detail::split_connected_smiles(smiles);
        if (components.empty()) { ++skipped; continue; }
        total_components += components.size();
        bool target_fragmented = true;
        for (std::size_t component_index = 0; component_index < components.size(); ++component_index)
        {
            if (access.is_cancelled()) throw std::runtime_error("Operation cancelled");
            streamfind::mass_spec::fragmentation::detail::TemporaryDirectory temp;
            const auto process = streamfind::mass_spec::fragmentation::detail::run_cli(
                *java, *jar, components[component_index], limits.depth,
                limits.timeout_seconds, limits.output_limit_bytes,
                temp.path() / "stdout.json", temp.path() / "stderr.log",
                [&access]() { return access.is_cancelled(); });
            if (process.cancelled) throw std::runtime_error("Operation cancelled");
            if (process.output_limit_exceeded)
            {
                target_fragmented = false;
                ++failed_components;
                errors.push_back({{"name", name}, {"component_index", component_index},
                    {"timed_out", false}, {"output_limit_exceeded", true},
                    {"message", "MetFrag Fragmenter output exceeded the configured fragmenter_output_limit_mb (" +
                        std::to_string(limits.output_limit_bytes / (1024ULL * 1024ULL)) + " MiB)."},
                    {"stderr_excerpt", process.error}});
                continue;
            }
            Json document = Json::parse(process.output, nullptr, false);
            if (process.exit_code != 0 || process.timed_out || document.is_discarded() || !document.is_object() ||
                !document.contains("fragments") || !document.at("fragments").is_array())
            {
                target_fragmented = false;
                ++failed_components;
                errors.push_back({{"name", name}, {"component_index", component_index},
                    {"exit_code", process.exit_code},
                    {"timed_out", process.timed_out},
                    {"message", process.timed_out ? "MetFrag Fragmenter timed out." :
                        (process.error.empty() ? "MetFrag Fragmenter returned a nonzero exit code or invalid JSON." : process.error)},
                    {"stderr_excerpt", process.error}});
                continue;
            }
            ++fragmented_components;
            for (const auto &fragment : document.at("fragments"))
            {
                if (rows.size() >= limits.maximum_fragment_rows)
                    throw std::runtime_error("MetFrag Fragmenter exceeded maximum_fragment_rows (" +
                        std::to_string(limits.maximum_fragment_rows) + "); no partial result was published.");
                const auto atom_indices = fragment.value("atom_indices", Json::array());
                const auto broken_bond_indices = fragment.value("broken_bond_indices", Json::array());
                const auto neutral_losses = fragment.value("neutral_losses", Json::array());
                const std::string raw_id = fragment.contains("id") ? streamfind::mass_spec::fragmentation::detail::json_scalar_text(fragment.at("id")) : std::string{};
                const std::string raw_parent_id = fragment.contains("parent_id") ? streamfind::mass_spec::fragmentation::detail::json_scalar_text(fragment.at("parent_id")) : std::string{};
                Json fragment_data = fragment;
                fragment_data["source_smiles"] = components[component_index];
                fragment_data["source_fragment_id"] = raw_id;
                fragment_data["source_parent_id"] = raw_parent_id;
                fragment_data["component_index"] = component_index;
                fragment_data["input_smiles"] = smiles;
                fragment_data["explicit_deuterium"] = smiles.find("[2H]") != std::string::npos;
                const std::string fragment_id = std::to_string(component_index) + ":" + raw_id;
                const Json parent_id = raw_parent_id.empty() ? Json(nullptr) : Json(std::to_string(component_index) + ":" + raw_parent_id);
                const int fragment_depth = fragment.value("depth", 0);
                rows.push_back({
                    {"name", name}, {"fragment_id", fragment_id}, {"parent_id", parent_id},
                    {"SMILES", fragment_depth == 0 ? components[component_index] : fragment.value("smiles", std::string{})},
                    {"formula", fragment.value("formula", std::string{})}, {"mass", fragment.value("exact_mass", 0.0)},
                    {"depth", fragment_depth}, {"atom_indices", atom_indices},
                    {"broken_bond_indices", broken_bond_indices}, {"neutral_losses", neutral_losses},
                    {"fragment_data", fragment_data}});
            }
            access.report_progress(
                static_cast<double>(component_index + 1) / static_cast<double>(components.size()),
                "Fragmented " + name + " component " + std::to_string(component_index + 1) + "/" + std::to_string(components.size()) + ".");
        }
        if (target_fragmented) ++fragmented;
    }
    if (fragmented_components == 0 && failed_components > 0)
        throw std::runtime_error("MetFrag Fragmenter failed for every structure component; no fragmentation result was published. First error: " +
            errors.front().value("message", std::string("unknown fragmenter failure")));
    access.emit_table_rows("fragmentationTreeTable", columns, types, rows);
    return {{"status", errors.empty() ? "finished" : "partial"},
            {"target_rows", targets.size()}, {"fragmented_targets", fragmented},
            {"skipped_without_smiles", skipped}, {"failed_targets", errors.size()},
            {"total_components", total_components}, {"fragmented_components", fragmented_components},
            {"failed_components", failed_components},
            {"fragment_rows", rows.size()}, {"depth", limits.depth},
            {"timeout_seconds", limits.timeout_seconds},
            {"output_limit_bytes", limits.output_limit_bytes},
            {"maximum_fragment_rows", limits.maximum_fragment_rows},
            {"fragmentation_method", "MetFrag top-down per-depth traversal"},
            {"artifact", "fragmentationTreeTable"}, {"errors", errors}};
}
} // namespace streamfind::mass_spec::fragmentation::fragment_suspect_targets
