#include "streamfind/dev/command_protocol.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace streamfind::dev::contract_test {

int run() {
    const char command[] = "streamfind-dev";
    const char subcommand[] = "run";
    const char program_option[] = "--program";
    const auto program = std::filesystem::absolute(".").lexically_normal().string();
    char *argv[] = {const_cast<char *>(command), const_cast<char *>(subcommand),
                    const_cast<char *>(program_option), const_cast<char *>(program.c_str()), nullptr};
    const auto request = parse_request(4, argv);
    if (request.command != "run" || !request.program || request.program->string() != program)
        throw std::runtime_error("run request parsing failed");
    const auto home = resolve_managed_home();
    const auto workspace = resolve_workspace(std::nullopt, home);
    bool denied = false;
    const auto outside =
#ifdef _WIN32
        std::filesystem::path("C:/Windows");
#else
        std::filesystem::path("/etc");
#endif
    try { (void)canonicalize_user_path(outside, false, home, workspace); }
    catch (const PathDeniedError &) { denied = true; }
    if (!denied) throw std::runtime_error("path policy accepted an escaping path");
    const char viewer_command[] = "streamfind-dev";
    const char viewer_subcommand[] = "viewer";
    const char viewer_action[] = "activate";
    const char viewer_id[] = "@streamfind/example-viewer";
    const char version_option[] = "--version";
    const char version[] = "0.1.0";
    char *viewer_argv[] = {const_cast<char *>(viewer_command), const_cast<char *>(viewer_subcommand),
                           const_cast<char *>(viewer_action), const_cast<char *>(viewer_id),
                           const_cast<char *>(version_option), const_cast<char *>(version), nullptr};
    const auto viewer_request = parse_request(6, viewer_argv);
    if (viewer_request.command != "viewer" || viewer_request.plugin_action != "activate" ||
        viewer_request.name != viewer_id || viewer_request.version != version)
        throw std::runtime_error("viewer request parsing failed");
    return 0;
}

} // namespace streamfind::dev::contract_test

int main() {
    try { return streamfind::dev::contract_test::run(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
