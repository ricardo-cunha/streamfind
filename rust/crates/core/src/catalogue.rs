//! Read-only access to the semantic catalogue knowledge base
//! (`semantic/generated/catalogue.duckdb`).
//!
//! The catalogue is the runtime replacement for the former embedded
//! `generated_metadata.rs` literals. Every method/operation document is one
//! row of `catalogue_entries`; this module opens the database read-only and
//! serves the MCP tool list and the registration views.
//!
//! Search chain:
//! 1. `STREAMFIND_CATALOGUE` environment variable (explicit override)
//! 2. `catalogue.duckdb` next to the executable
//! 3. the repository source-tree layout (`semantic/generated`) — dev/test
//!    fallback anchored at the core crate manifest dir

use crate::{
    Method, MethodExecutor, MethodRegistry, MethodValidator, Operation, OperationExecutor,
    OperationRegistry, OperationValidator, ParameterSchema,
};
use duckdb::{AccessMode, Config, Connection};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::sync::OnceLock;

/// Explicit executable binding for one semantic workflow method.
pub struct MethodBinding {
    pub id: &'static str,
    pub executor: MethodExecutor,
    pub validator: Option<MethodValidator>,
}

/// Explicit executable binding for one semantic public operation.
pub struct OperationBinding {
    pub id: &'static str,
    pub executor: OperationExecutor,
    pub validator: Option<OperationValidator>,
}

/// Native composition boundary for one semantic domain module.
pub struct DomainModuleBinding {
    pub module_id: &'static str,
    pub domain_id: &'static str,
    pub module_version: &'static str,
    pub required_modules: Vec<&'static str>,
    pub methods: Vec<MethodBinding>,
    pub operations: Vec<OperationBinding>,
    pub tables: Vec<String>,
    pub schema_binding: Option<Box<dyn Fn() + Send + Sync>>,
}

const CATALOGUE_SQL: &str = "SELECT canonical_id, kind, domain, label, definition, category, \
     invocation_model, requires_connection, guidance, CAST(next_operations AS VARCHAR), \
     interface_guidance, executable, exposed, mcp_name, CAST(input_schema AS VARCHAR), \
     CAST(parameters AS VARCHAR), CAST(result_schema AS VARCHAR), CAST(reads_tables AS VARCHAR), \
     CAST(writes_tables AS VARCHAR), cacheable, single_occurrence, mutates_project, \
     CAST(required_methods AS VARCHAR), module_id \
     FROM catalogue_entries ORDER BY canonical_id";

fn env_path() -> Option<PathBuf> {
    std::env::var_os("STREAMFIND_CATALOGUE").map(PathBuf::from)
}

fn binary_relative() -> Option<PathBuf> {
    let candidate = std::env::current_exe()
        .ok()?
        .parent()?
        .join("catalogue.duckdb");
    candidate.exists().then_some(candidate)
}

/// Release/install layout: <prefix>/bin/<exe> + <prefix>/share/streamfind/.
/// Resolving relative to the executable makes unpacked release archives
/// relocatable (no compile-time prefix dependency).
fn binary_relative_share() -> Option<PathBuf> {
    let candidate = std::env::current_exe()
        .ok()?
        .parent()?
        .parent()?
        .join("share")
        .join("streamfind")
        .join("catalogue.duckdb");
    candidate.exists().then_some(candidate)
}

fn repo_layout() -> Option<PathBuf> {
    let candidate = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../semantic/generated/catalogue.duckdb");
    candidate.exists().then_some(candidate)
}

/// Resolve `catalogue.duckdb` via the runtime search chain.
pub fn find_path() -> Option<PathBuf> {
    env_path()
        .or_else(binary_relative)
        .or_else(binary_relative_share)
        .or_else(repo_layout)
}

fn parse_json(raw: Option<String>, id: &str) -> Result<Value, String> {
    match raw {
        None => Ok(Value::Null),
        Some(text) => serde_json::from_str(&text)
            .map_err(|error| format!("catalogue: invalid JSON for {id}: {error}")),
    }
}

fn load_entries() -> Result<Vec<Value>, String> {
    let path = find_path().ok_or_else(|| {
        "catalogue.duckdb not found; searched STREAMFIND_CATALOGUE, the executable directory, \
         and the repository semantic/generated layout. Ensure the runtime knowledge base is \
         installed alongside the binaries."
            .to_string()
    })?;
    let config = Config::default()
        .access_mode(AccessMode::ReadOnly)
        .map_err(|error| format!("catalogue: config failed: {error}"))?;
    let connection = Connection::open_with_flags(&path, config)
        .map_err(|error| format!("catalogue: failed to open {}: {error}", path.display()))?;
    let mut statement = connection
        .prepare(CATALOGUE_SQL)
        .map_err(|error| format!("catalogue: prepare failed: {error}"))?;
    let mut rows = statement
        .query([])
        .map_err(|error| format!("catalogue: query failed: {error}"))?;
    let mut entries = Vec::new();
    while let Some(row) = rows
        .next()
        .map_err(|error| format!("catalogue: row read failed: {error}"))?
    {
        let canonical_id: String = row
            .get(0)
            .map_err(|error| format!("catalogue: canonical_id read failed: {error}"))?;
        let kind: String = row
            .get(1)
            .map_err(|error| format!("catalogue: kind read failed: {error}"))?;
        let entry = json!({
            "kind": kind,
            "canonical_id": canonical_id,
            "domain": row.get::<_, String>(2).map_err(|e| e.to_string())?,
            "module_id": row.get::<_, String>(23).map_err(|e| e.to_string())?,
            "label": row.get::<_, String>(3).map_err(|e| e.to_string())?,
            "definition": row.get::<_, String>(4).map_err(|e| e.to_string())?,
            "interface": json!({
                "category": row.get::<_, String>(5).map_err(|e| e.to_string())?,
                "invocation_model": row.get::<_, String>(6).map_err(|e| e.to_string())?,
                "requires_connection": row.get::<_, bool>(7).map_err(|e| e.to_string())?,
                "guidance": row.get::<_, String>(8).map_err(|e| e.to_string())?,
                "next_operations": parse_json(row.get::<_, Option<String>>(9).map_err(|e| e.to_string())?, &canonical_id)?,
            }),
            "interface_guidance": row.get::<_, String>(10).map_err(|e| e.to_string())?,
            "executable": row.get::<_, bool>(11).map_err(|e| e.to_string())?,
            "exposed": row.get::<_, bool>(12).map_err(|e| e.to_string())?,
            "parameters": parse_json(row.get::<_, Option<String>>(15).map_err(|e| e.to_string())?, &canonical_id)?,
            "result": json!({"schema": parse_json(row.get::<_, Option<String>>(16).map_err(|e| e.to_string())?, &canonical_id)?}),
            "effects": json!({
                "mutates_project": row.get::<_, bool>(21).map_err(|e| e.to_string())?,
                "reads": parse_json(row.get::<_, Option<String>>(17).map_err(|e| e.to_string())?, &canonical_id)?,
                "writes": parse_json(row.get::<_, Option<String>>(18).map_err(|e| e.to_string())?, &canonical_id)?,
            }),
        });
        let input_schema = parse_json(
            row.get::<_, Option<String>>(14)
                .map_err(|error| format!("catalogue: input_schema read failed: {error}"))?,
            &canonical_id,
        )?;
        let entry = if kind == "operation" {
            let name: Option<String> = row
                .get(13)
                .map_err(|error| format!("catalogue: mcp_name read failed: {error}"))?;
            let mut entry = entry;
            entry["mcp"] = json!({"name": name.unwrap_or_else(|| canonical_id.clone()), "input_schema": input_schema});
            entry
        } else {
            let mut entry = entry;
            entry["method_schema"] = input_schema;
            entry["cacheable"] = json!(row.get::<_, bool>(19).map_err(|e| e.to_string())?);
            entry["single_occurrence"] = json!(row.get::<_, bool>(20).map_err(|e| e.to_string())?);
            entry["required_methods"] = parse_json(
                row.get::<_, Option<String>>(22)
                    .map_err(|e| e.to_string())?,
                &canonical_id,
            )?;
            entry
        };
        entries.push(entry);
    }
    Ok(entries)
}

struct Catalogue {
    entries: Vec<Value>,
    error: Option<String>,
}

fn catalogue() -> &'static Catalogue {
    static CATALOGUE: OnceLock<Catalogue> = OnceLock::new();
    CATALOGUE.get_or_init(|| match load_entries() {
        Ok(entries) => Catalogue {
            entries,
            error: None,
        },
        Err(error) => Catalogue {
            entries: Vec::new(),
            error: Some(error),
        },
    })
}

/// All catalogue entry documents (same shape as catalogue.json `entries`).
/// Empty when the catalogue cannot be located or read.
pub fn entries() -> &'static [Value] {
    &catalogue().entries
}

/// MCP tool definitions: exposed streamfind operations shaped
/// {name, description, inputSchema, outputSchema, effects} — the same shape
/// the generated `TOOLS` literal provided. Empty on a catalogue miss.
pub fn tools_json() -> Value {
    let tools = entries()
        .iter()
        .filter(|entry| {
            entry["kind"] == "operation"
                && entry["domain"] == "streamfind"
                && entry["exposed"].as_bool() == Some(true)
        })
        .map(|entry| {
            let mut description = format!(
                "{}: {}",
                entry["label"].as_str().unwrap_or("streamfind operation"),
                entry["definition"].as_str().unwrap_or("")
            );
            if let Some(guidance) = entry["interface"]["guidance"].as_str() {
                if !guidance.is_empty() {
                    description.push_str(" Guidance: ");
                    description.push_str(guidance);
                }
            }
            if let Some(model) = entry["interface"]["invocation_model"].as_str() {
                description.push_str(" Invocation model: ");
                description.push_str(model);
                description.push('.');
            }
            let read_only = entry["effects"]["mutates_project"] == false;
            json!({
                "name": entry["mcp"]["name"],
                "description": description,
                "inputSchema": entry["mcp"]["input_schema"],
                "annotations": {
                    "title": entry["label"],
                    "readOnlyHint": read_only,
                    "destructiveHint": !read_only,
                },
                "_meta": {"streamfind": entry["interface"]},
                // No outputSchema: MCP requires it to be a JSON-Schema object
                // AND that every result carries matching structuredContent.
                // streamfind results are table-like documents returned as text
                // content, so omitting outputSchema is the spec-correct shape
                // (it is optional in MCP). The semantic result schema remains
                // available via the catalogue query operations.
                "effects": entry["effects"],
            })
        })
        .collect::<Vec<_>>();
    Value::Array(tools)
}

/// Method contract documents (kind='method'), for available-methods queries.
pub fn methods_json() -> Value {
    let methods = entries()
        .iter()
        .filter(|entry| entry["kind"] == "method")
        .cloned()
        .collect::<Vec<_>>();
    Value::Array(methods)
}

/// Return the semantic module that provides a canonical capability.
pub fn module_id(id: &str) -> Option<&'static str> {
    entries()
        .iter()
        .find(|entry| entry["canonical_id"] == id)
        .and_then(|entry| entry["module_id"].as_str())
}

/// Load generated table contracts for one semantic domain.
pub fn table_manifest(domain: &str) -> Result<Vec<(String, Vec<(String, String)>)>, String> {
    let path = find_path().ok_or_else(|| "catalogue.duckdb not found".to_owned())?;
    let config = Config::default()
        .access_mode(AccessMode::ReadOnly)
        .map_err(|error| error.to_string())?;
    let connection =
        Connection::open_with_flags(&path, config).map_err(|error| error.to_string())?;
    let mut statement = connection
        .prepare("SELECT table_name, CAST(columns AS VARCHAR) FROM catalogue_tables WHERE domain = ? ORDER BY table_name")
        .map_err(|error| error.to_string())?;
    let mut rows = statement
        .query([domain])
        .map_err(|error| error.to_string())?;
    let mut tables = Vec::new();
    while let Some(row) = rows.next().map_err(|error| error.to_string())? {
        let name: String = row.get(0).map_err(|error| error.to_string())?;
        let columns: String = row.get(1).map_err(|error| error.to_string())?;
        let columns: Vec<Value> =
            serde_json::from_str(&columns).map_err(|error| error.to_string())?;
        tables.push((
            name,
            columns
                .into_iter()
                .filter_map(|column| {
                    Some((
                        column["name"].as_str()?.to_owned(),
                        column["type"].as_str().unwrap_or_default().to_owned(),
                    ))
                })
                .collect(),
        ));
    }
    Ok(tables)
}

/// Register executable catalogue entries through domain-provided bindings.
pub fn register_methods_from_catalogue<S, R>(
    registry: &mut MethodRegistry,
    domain: &str,
    schema_for: S,
    resolver: R,
) -> crate::Result<()>
where
    S: Fn(&Value) -> ParameterSchema,
    R: Fn(&str) -> Option<MethodExecutor>,
{
    for entry in entries().iter().filter(|entry| {
        entry["kind"] == "method"
            && entry["domain"].as_str() == Some(domain)
            && entry["executable"].as_bool().unwrap_or(false)
    }) {
        let id = entry["canonical_id"].as_str().unwrap_or_default();
        let Some(executor) = resolver(id) else {
            continue;
        };
        let mut method = Method::new(
            id,
            entry["label"].as_str().unwrap_or(id),
            entry["definition"].as_str().unwrap_or_default(),
            domain,
            schema_for(entry),
            executor,
        );
        method.cacheable = entry["cacheable"].as_bool().unwrap_or(false);
        method.single_occurrence = entry["single_occurrence"].as_bool().unwrap_or(false);
        registry.register(method)?;
    }
    Ok(())
}

pub fn register_methods_from_catalogue_with<S, R, D>(
    registry: &mut MethodRegistry,
    domain: &str,
    schema_for: S,
    resolver: R,
    decorate: D,
) -> crate::Result<()>
where
    S: Fn(&Value) -> ParameterSchema,
    R: Fn(&str) -> Option<MethodExecutor>,
    D: Fn(Method) -> Method,
{
    for entry in entries().iter().filter(|entry| {
        entry["kind"] == "method"
            && entry["domain"].as_str() == Some(domain)
            && entry["executable"].as_bool().unwrap_or(false)
    }) {
        let id = entry["canonical_id"].as_str().unwrap_or_default();
        let Some(executor) = resolver(id) else {
            continue;
        };
        let mut method = Method::new(
            id,
            entry["label"].as_str().unwrap_or(id),
            entry["definition"].as_str().unwrap_or_default(),
            domain,
            schema_for(entry),
            executor,
        );
        method.cacheable = entry["cacheable"].as_bool().unwrap_or(false);
        method.single_occurrence = entry["single_occurrence"].as_bool().unwrap_or(false);
        registry.register(decorate(method))?;
    }
    Ok(())
}

/// Register executable operation entries through domain-provided bindings.
pub fn register_operations_from_catalogue<S, R>(
    registry: &mut OperationRegistry,
    domain: &str,
    schema_for: S,
    resolver: R,
) -> crate::Result<()>
where
    S: Fn(&Value) -> ParameterSchema,
    R: Fn(&str) -> Option<OperationExecutor>,
{
    for entry in entries().iter().filter(|entry| {
        entry["kind"] == "operation"
            && entry["domain"].as_str() == Some(domain)
            && entry["executable"].as_bool().unwrap_or(false)
    }) {
        let id = entry["canonical_id"].as_str().unwrap_or_default();
        let Some(executor) = resolver(id) else {
            continue;
        };
        registry.register(Operation::new(
            id,
            entry["label"].as_str().unwrap_or(id),
            entry["definition"].as_str().unwrap_or_default(),
            domain,
            schema_for(entry),
            executor,
        ))?;
    }
    Ok(())
}

/// Register one explicit domain module and verify semantic module ownership.
pub fn register_module<S>(
    module: DomainModuleBinding,
    method_registry: &mut MethodRegistry,
    operation_registry: &mut OperationRegistry,
    schema_for: S,
) -> crate::Result<()>
where
    S: Fn(&Value) -> ParameterSchema,
{
    register_module_with_entries(
        entries(),
        module,
        method_registry,
        operation_registry,
        schema_for,
    )
}

/// Register one explicit module against a caller-provided catalogue snapshot.
pub fn register_module_with_entries<S>(
    entries: &[Value],
    module: DomainModuleBinding,
    method_registry: &mut MethodRegistry,
    operation_registry: &mut OperationRegistry,
    schema_for: S,
) -> crate::Result<()>
where
    S: Fn(&Value) -> ParameterSchema,
{
    for dependency in &module.required_modules {
        if !entries
            .iter()
            .any(|entry| entry["module_id"] == *dependency)
        {
            return Err(crate::Error::new(
                crate::ErrorCode::InvalidArgument,
                format!(
                    "catalogue: missing module dependency {dependency} for module {}",
                    module.module_id
                ),
            ));
        }
    }
    if !module.tables.is_empty() {
        let manifest = table_manifest(&module.domain_id)
            .map_err(|error| crate::Error::new(crate::ErrorCode::InvalidArgument, error))?;
        for table in &module.tables {
            if !manifest.iter().any(|(name, _)| name == table) {
                return Err(crate::Error::new(
                    crate::ErrorCode::InvalidArgument,
                    format!(
                        "catalogue: missing owned table {table} for module {}",
                        module.module_id
                    ),
                ));
            }
        }
    }
    for binding in module.methods {
        let entry = entries
            .iter()
            .find(|entry| {
                entry["kind"] == "method"
                    && entry["canonical_id"].as_str() == Some(binding.id)
                    && entry["domain"].as_str() == Some(module.domain_id)
                    && entry["module_id"].as_str() == Some(module.module_id)
            })
            .ok_or_else(|| {
                crate::Error::new(
                    crate::ErrorCode::InvalidArgument,
                    format!("invalid method binding: {}", binding.id),
                )
            })?;
        let mut method = Method::new(
            binding.id,
            entry["label"].as_str().unwrap_or(binding.id),
            entry["definition"].as_str().unwrap_or_default(),
            module.domain_id,
            schema_for(entry),
            binding.executor,
        );
        method.cacheable = entry["cacheable"].as_bool().unwrap_or(false);
        method.single_occurrence = entry["single_occurrence"].as_bool().unwrap_or(false);
        method.writes = entry["effects"]["writes"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .map(str::to_owned)
            .collect();
        method.required_methods = entry["required_methods"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .map(str::to_owned)
            .collect();
        if let Some(validator) = binding.validator {
            method = method.with_validator(validator);
        }
        method_registry.register(method)?;
    }
    for binding in module.operations {
        let entry = entries
            .iter()
            .find(|entry| {
                entry["kind"] == "operation"
                    && entry["canonical_id"].as_str() == Some(binding.id)
                    && entry["domain"].as_str() == Some(module.domain_id)
                    && entry["module_id"].as_str() == Some(module.module_id)
            })
            .ok_or_else(|| {
                crate::Error::new(
                    crate::ErrorCode::InvalidArgument,
                    format!("invalid operation binding: {}", binding.id),
                )
            })?;
        let mut operation = Operation::new(
            binding.id,
            entry["label"].as_str().unwrap_or(binding.id),
            entry["definition"].as_str().unwrap_or_default(),
            module.domain_id,
            schema_for(entry),
            binding.executor,
        );
        if let Some(validator) = binding.validator {
            operation = operation.with_validator(validator);
        }
        operation_registry.register(operation)?;
    }
    Ok(())
}

/// Shared project-management and workflow guidance returned by MCP initialize.
/// It is sourced from the streamfind domain entry in the generated catalogue.
pub fn interface_guidance() -> String {
    entries()
        .iter()
        .find_map(|entry| entry["interface_guidance"].as_str())
        .filter(|value| !value.is_empty())
        .unwrap_or(
            "Start with create, then describe the project. Domain operations are stateless and require database_path; connect is only needed for workflow methods.",
        )
        .to_owned()
}
pub fn load_error() -> Option<&'static str> {
    catalogue().error.as_deref()
}
