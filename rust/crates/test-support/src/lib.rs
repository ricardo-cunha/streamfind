//! Test-support helpers, per AGENTS.md "Repository Scratch, Build, and Log
//! Locations (tmp/)".
//!
//! Every transient artifact a test or ad-hoc run creates must live under the
//! repository-local `tmp/` folder — never in the system temp directory
//! (`std::env::temp_dir()`), the CWD, or the source tree. This crate provides
//! the canonical path for test-created DuckDB projects and other disposable
//! files: `<repo-root>/tmp/projects/`.

use std::path::PathBuf;

/// Returns `<repo-root>/tmp/projects`, creating it (and any missing parents)
/// on first use.
///
/// The repository root is located from this crate's expected position inside
/// the workspace (`rust/crates/test-support/src/`), so the helper works
/// regardless of the test executable's working directory.
pub fn tmp_projects_dir() -> PathBuf {
    let here = PathBuf::from(env!("CARGO_MANIFEST_DIR")); // rust/crates/test-support
    let repo = here
        .parent() // rust/crates
        .and_then(|p| p.parent()) // rust
        .and_then(|p| p.parent()) // <repo-root>
        .expect("test-support crate must live at rust/crates/test-support");
    let dir = repo.join("tmp").join("projects");
    std::fs::create_dir_all(&dir).expect("cannot create repository tmp/projects directory");
    dir
}

/// Returns the repository root (`<repo-root>`), located from this crate's
/// expected position inside the workspace.
pub fn repo_root() -> PathBuf {
    let here = PathBuf::from(env!("CARGO_MANIFEST_DIR")); // rust/crates/test-support
    here.parent() // rust/crates
        .and_then(|p| p.parent()) // rust
        .and_then(|p| p.parent()) // <repo-root>
        .expect("test-support crate must live at rust/crates/test-support")
        .to_path_buf()
}

/// Parses the committed semantic catalogue (`semantic/generated/catalogue.json`)
/// and returns its `entries` array. Tests derive expectations from this file —
/// the same catalogue the MCP servers serve from — so they adapt automatically
/// when operations/methods are added or removed, instead of hardcoding counts.
pub fn catalogue_entries() -> Vec<serde_json::Value> {
    let path = repo_root()
        .join("semantic")
        .join("generated")
        .join("catalogue.json");
    let text = std::fs::read_to_string(&path).unwrap_or_else(|error| {
        panic!(
            "cannot read semantic catalogue at {}: {error}",
            path.display()
        )
    });
    let catalogue: serde_json::Value = serde_json::from_str(&text).unwrap_or_else(|error| {
        panic!(
            "cannot parse semantic catalogue at {}: {error}",
            path.display()
        )
    });
    catalogue["entries"].as_array().cloned().unwrap_or_default()
}

/// The catalogue entries that MCP advertises as callable tools: exposed
/// operations of the `streamfind` domain (`tools/list` filters exactly on this).
pub fn advertised_core_tools() -> Vec<serde_json::Value> {
    catalogue_entries()
        .into_iter()
        .filter(|entry| {
            entry["kind"] == "operation"
                && entry["domain"] == "streamfind"
                && entry["exposed"] == true
        })
        .collect()
}
