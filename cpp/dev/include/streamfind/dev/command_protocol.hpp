#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

namespace streamfind::dev {

struct PathDeniedError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

enum class ErrorCode : int {
    InvalidArguments = 2,
    PathDenied = 3,
    SpawnFailed = 4,
    Timeout = 5,
    ChildFailed = 6,
    Cancelled = 7,
};

struct Request {
    std::string command;
    std::string plugin_action;
    std::string name;
    std::string example;
    bool json{false};
    bool offline{false};
    bool dry_run{false};
    std::string target{"all"};
    std::optional<std::filesystem::path> workspace;
    std::optional<std::filesystem::path> managed_home;
    std::optional<std::filesystem::path> sdk;
    std::optional<std::filesystem::path> package;
    std::optional<std::filesystem::path> viewer;
    std::optional<std::filesystem::path> plugin_path;
    std::optional<std::filesystem::path> build_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::filesystem::path> program;
    std::optional<std::filesystem::path> working_directory;
    std::optional<std::filesystem::path> output_file;
    std::optional<std::filesystem::path> requirements;
    std::vector<std::string> program_arguments;
    std::string config{"Release"};
    std::string version;
    std::string contract;
    std::size_t timeout_ms{30000};
};

struct ProcessResult {
    int exit_code{0};
    bool timed_out{false};
    bool cancelled{false};
    bool output_truncated{false};
    std::string output;
};

Request parse_request(int argc, char **argv);
std::filesystem::path resolve_managed_home();
std::filesystem::path resolve_workspace(const std::optional<std::filesystem::path> &requested,
                                        const std::filesystem::path &managed_home);
std::filesystem::path canonicalize_user_path(const std::filesystem::path &value,
                                              bool must_exist,
                                              const std::filesystem::path &managed_home,
                                              const std::filesystem::path &workspace);
ProcessResult run_process(const std::filesystem::path &program,
                          const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory,
                          std::size_t timeout_ms,
                          std::size_t output_limit,
                          const std::optional<std::filesystem::path> &output_file,
                          bool isolate_environment,
                          const std::vector<std::string> &environment_overrides = {},
                          const std::function<bool()> &cancel_requested = {});

void install_cancellation_handler();

std::string doctor_json(const Request &request);
std::string usage();

int scaffold_plugin(const Request &request);
int add_plugin_operation(const Request &request);
int build_plugin(const Request &request);
int test_plugin(const Request &request);
int validate_plugin(const Request &request);
int package_plugin(const Request &request);
int rebuild_plugin(const Request &request);
int install_plugin(const Request &request);
int uninstall_plugin(const Request &request);
int deactivate_plugin(const Request &request);
int activate_plugin(const Request &request);
int install_viewer(const Request &request);
int uninstall_viewer(const Request &request);
int deactivate_viewer(const Request &request);
int activate_viewer(const Request &request);
int scaffold_viewer(const Request &request);
int build_viewer(const Request &request);
int test_viewer(const Request &request);
int validate_viewer(const Request &request);
int package_viewer(const Request &request);
int list_extensions(const Request &request);
int inspect_extension(const Request &request);

} // namespace streamfind::dev
