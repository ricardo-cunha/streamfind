//! Standalone Rust backend for DuckDB-backed Streamfind projects.
//!
//! The [`Project`] type owns project metadata, workflow definitions, cache
//! records, audit records, parameter validation, and workflow execution.
//! Projects use the same DuckDB schema as the C++ backend.

pub mod api;
pub mod catalogue;
pub mod project;
pub mod project_table_store;

pub use catalogue::{DomainModuleBinding, MethodBinding, OperationBinding};
pub use project::*;
pub use project_table_store::{ProjectTableStore, TableRequirement};
