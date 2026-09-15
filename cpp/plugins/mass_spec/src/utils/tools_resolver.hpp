#ifndef streamfind_MASS_SPEC_TOOLS_RESOLVER_H
#define streamfind_MASS_SPEC_TOOLS_RESOLVER_H

#include <optional>
#include <string>

#include "streamfind/export.hpp"

namespace streamfind::mass_spec::tools {

/// `%USERPROFILE%\.streamfind` on Windows or `$HOME/.streamfind` on Linux/macOS.
STREAMFIND_DOMAIN_API std::string streamfind_home();

/// `<home>/tools`.
STREAMFIND_DOMAIN_API std::string tools_dir();

/// Locates `java`/`java.exe`: PATH -> `JAVA_HOME/bin` ->
/// `<tools>/java/jdk-*/bin` (R rule: PATH first).
STREAMFIND_DOMAIN_API std::optional<std::string> resolve_java();

/// Locates the MetFrag command-line jar: `<tools>/metfrag/MetFragCL.jar`.
STREAMFIND_DOMAIN_API std::optional<std::string> resolve_metfrag_jar();

/// `java` + jar when both are installed.
STREAMFIND_DOMAIN_API std::optional<std::pair<std::string, std::string>> resolve_metfrag();

/// Installs Temurin JDK 21 into `<tools>/java/` using staged extraction and an atomic directory move.
STREAMFIND_DOMAIN_API std::string install_java();

/// Explicit setup check for MetFrag. Does not download or modify the filesystem.
STREAMFIND_DOMAIN_API std::string install_metfrag();

/// Human-readable tool status (paths or "not found").
STREAMFIND_DOMAIN_API std::string tool_status();

} // namespace streamfind::mass_spec::tools

#endif // streamfind_MASS_SPEC_TOOLS_RESOLVER_H