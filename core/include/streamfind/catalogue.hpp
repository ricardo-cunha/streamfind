#pragma once

#include "streamfind/project.hpp"

#include <optional>
#include <string>

/**
 * @file catalogue.hpp
 * @brief Read-only access to the semantic catalogue knowledge base
 *        (semantic/generated/catalogue.duckdb).
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

/**
 * @brief Load (and cache) the catalogue entries as a JSON array of entry
 *        documents (same shape as catalogue.json `entries`).
 * @param path Explicit database path; when nullopt the runtime search chain
 *        is used.
 * @return The entries array, or nullopt when the database cannot be located
 *         or read (see load_error() for the reason).
 */
STREAMFIND_CORE_API std::optional<Json> load(const std::optional<std::string> &path = std::nullopt);

/**
 * @brief MCP tool definitions: exposed operations (kind='operation') shaped
 *        {name, description, inputSchema, outputSchema, effects} — the same
 *        shape the generated tools literal provided.
 */
STREAMFIND_CORE_API std::optional<Json> tools_json();

/** @brief All entry documents, for the registry auto-registration loops. */
STREAMFIND_CORE_API std::optional<Json> entries_json();
STREAMFIND_CORE_API std::optional<Json> table_manifest_json(const std::string &domain);

/**
 * @brief Method contract documents (kind='method'), for available-methods
 *        queries and workflow assembly.
 */
STREAMFIND_CORE_API std::optional<Json> methods_json();

/** @brief Human-readable reason for the most recent load failure. */
STREAMFIND_CORE_API std::string load_error();

}  // namespace streamfind::catalogue