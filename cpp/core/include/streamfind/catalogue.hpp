#pragma once

#include "streamfind/project.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file catalogue.hpp
 * @brief Read-only access to the semantic catalogue knowledge base
 *        (the native build-tree semantic catalogue).
 *
 * The catalogue is the runtime replacement for the former embedded
 * generated_metadata.hpp literals. Every method/operation document is one row
 * of `catalogue_entries`; this module opens the database read-only (search
 * chain: STREAMFIND_CATALOGUE env -> next to the executable -> install data
 * dir), reconstructs the entry documents, and serves the MCP tool list and the
 * registration views.
 */

namespace streamfind::catalogue {

/** @brief Resolve the catalogue.duckdb path via the runtime search chain. */
STREAMFIND_CORE_API std::optional<std::string> find_path();
STREAMFIND_CORE_API std::optional<std::string> find_core_path();

/** @brief Pin the process catalogue path used by manifest lookups. */
STREAMFIND_CORE_API void set_runtime_path(const std::string &path);
STREAMFIND_CORE_API void set_runtime_document(Json document);
STREAMFIND_CORE_API std::optional<Json> load_document(const std::string &path);

/**
 * @brief Load (and cache) the catalogue entries as a JSON array of entry
 *        documents (same shape as catalogue.json `entries`).
 * @param path Explicit database path; when nullopt the runtime search chain
 *        is used.
 * @return The entries array, or nullopt when the database cannot be located
 *         or read (see load_error() for the reason).
 */
STREAMFIND_CORE_API std::optional<Json> load(const std::optional<std::string> &path = std::nullopt);

/** @brief Validate and merge one plugin catalogue into a base snapshot. */
STREAMFIND_CORE_API Json import_plugin_catalogue(
    const Json &base,
    const Json &plugin,
    std::string_view expected_domain,
    const std::vector<std::string> &expected_modules);

/**
 * @brief MCP tool definitions: exposed operations (kind='operation') shaped
 *        {name, description, inputSchema, outputSchema, effects} — the same
 *        shape the generated tools literal provided.
 */
STREAMFIND_CORE_API std::optional<Json> tools_json();

/** @brief All entry documents, for the registry auto-registration loops. */
STREAMFIND_CORE_API std::optional<Json> entries_json();
STREAMFIND_CORE_API std::optional<Json> table_manifest_json(const std::string &domain,
                                                            const std::string &module_id);

/**
 * @brief Method contract documents (kind='method'), for available-methods
 *        queries and workflow assembly.
 */
STREAMFIND_CORE_API std::optional<Json> methods_json();

/** @brief Human-readable reason for the most recent load failure. */
STREAMFIND_CORE_API std::string load_error();

}  // namespace streamfind::catalogue