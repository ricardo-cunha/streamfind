use duckdb::{params, types::Value as DuckValue, Config, Connection, OptionalExt};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};

pub type Json = Value;
const FRAMEWORK_VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorCode {
    InvalidArgument,
    ProjectNotFound,
    ProjectAlreadyExists,
    SchemaMismatch,
    DatabaseError,
    WorkflowValidation,
    MethodExecution,
    ProjectClosed,
    Cancelled,
}

/// Durable workflow execution lifecycle states.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExecutionState {
    Queued,
    Running,
    Cancelling,
    Cancelled,
    Completed,
    Failed,
    Interrupted,
}

/// Return whether a workflow execution lifecycle transition is allowed.
pub fn valid_execution_transition(from: ExecutionState, to: ExecutionState) -> bool {
    matches!(
        (from, to),
        (ExecutionState::Queued, ExecutionState::Running)
            | (ExecutionState::Queued, ExecutionState::Cancelled)
            | (ExecutionState::Running, ExecutionState::Completed)
            | (ExecutionState::Running, ExecutionState::Failed)
            | (ExecutionState::Running, ExecutionState::Cancelling)
            | (ExecutionState::Running, ExecutionState::Interrupted)
            | (ExecutionState::Cancelling, ExecutionState::Cancelled)
    )
}

#[derive(Debug)]
pub struct Error {
    pub code: ErrorCode,
    pub message: String,
}

impl Error {
    pub fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, output: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(output, "{:?}: {}", self.code, self.message)
    }
}

impl std::error::Error for Error {}

impl From<duckdb::Error> for Error {
    fn from(error: duckdb::Error) -> Self {
        Self::new(ErrorCode::DatabaseError, error.to_string())
    }
}

impl From<String> for Error {
    fn from(message: String) -> Self {
        Self::new(ErrorCode::InvalidArgument, message)
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Cooperative cancellation state for long-running operations.
#[derive(Default)]
pub struct CancellationToken {
    cancelled: std::sync::atomic::AtomicBool,
}

impl CancellationToken {
    pub fn cancel(&self) {
        self.cancelled
            .store(true, std::sync::atomic::Ordering::Relaxed);
    }
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(std::sync::atomic::Ordering::Relaxed)
    }
}

/// Progress snapshot emitted during workflow execution.
#[derive(Debug, Clone)]
pub struct ProgressEvent {
    pub operation: String,
    pub completed: usize,
    pub total: usize,
}

/// Stable result envelope for workflow execution.
#[derive(Debug, Clone, Serialize)]
pub struct ExecutionResult {
    pub results: Json,
    pub cancelled: bool,
}

impl ExecutionResult {
    pub fn to_json(&self) -> Json {
        json!({"results": self.results, "cancelled": self.cancelled})
    }
}

pub type ProgressCallback = Box<dyn Fn(&ProgressEvent) + Send + Sync>;

fn cache_key(previous: &str, method: &Method, parameters: &Json) -> String {
    let mut hash: u64 = 1469598103934665603;
    for byte in format!("{previous}\n{}\n1\n{}", method.id, parameters).bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(1099511628211);
    }
    format!("{hash:016x}")
}

fn duck_value_to_json(value: DuckValue) -> Json {
    match value {
        DuckValue::Null => Json::Null,
        DuckValue::Boolean(value) => json!(value),
        DuckValue::TinyInt(value) => json!(value),
        DuckValue::SmallInt(value) => json!(value),
        DuckValue::Int(value) => json!(value),
        DuckValue::BigInt(value) => json!(value),
        DuckValue::HugeInt(value) => json!(value.to_string()),
        DuckValue::UTinyInt(value) => json!(value),
        DuckValue::USmallInt(value) => json!(value),
        DuckValue::UInt(value) => json!(value),
        DuckValue::UBigInt(value) => json!(value),
        DuckValue::Float(value) => json!(value),
        DuckValue::Double(value) => json!(value),
        DuckValue::Decimal(value) => value
            .to_string()
            .parse::<f64>()
            .map(|value| json!(value))
            .unwrap_or_else(|_| json!(value.to_string())),
        DuckValue::Text(value) => Json::String(value),
        DuckValue::Blob(value) => json!(value),
        DuckValue::Timestamp(_, value) => json!(value),
        DuckValue::Date32(value) => json!(value),
        DuckValue::Time64(_, value) => json!(value),
        DuckValue::Interval {
            months,
            days,
            nanos,
        } => json!({"months": months, "days": days, "nanos": nanos}),
        DuckValue::List(values) | DuckValue::Array(values) => {
            Json::Array(values.into_iter().map(duck_value_to_json).collect())
        }
        DuckValue::Enum(value) => Json::String(value),
        DuckValue::Struct(values) => Json::Object(
            values
                .iter()
                .map(|(name, value)| (name.clone(), duck_value_to_json(value.clone())))
                .collect(),
        ),
        DuckValue::Map(values) => Json::Array(
            values
                .iter()
                .map(|(key, value)| {
                    json!([
                        duck_value_to_json(key.clone()),
                        duck_value_to_json(value.clone())
                    ])
                })
                .collect(),
        ),
        DuckValue::Union(value) => duck_value_to_json(*value),
        value => json!(format!("{value:?}")),
    }
}

fn quote_identifier(value: &str) -> String {
    format!("\"{}\"", value.replace('"', "\"\""))
}

fn sql_literal(value: &Json) -> String {
    match value {
        Json::Null => "NULL".into(),
        Json::Bool(value) => value.to_string().to_uppercase(),
        Json::Number(value) => value.to_string(),
        Json::String(value) => format!("'{}'", value.replace('\'', "''")),
        _ => format!("'{}'", value.to_string().replace('\'', "''")),
    }
}

fn snapshot_tables(connection: &Connection, tables: &[String]) -> Result<Json> {
    let mut snapshots = serde_json::Map::new();
    for table in tables {
        let filter = String::new();
        let mut statement = connection.prepare(&format!(
            "SELECT to_json(t) FROM {} t{}",
            quote_identifier(table),
            filter
        ))?;
        let mut rows = statement.query([])?;
        let mut snapshot = Vec::new();
        while let Some(row) = rows.next()? {
            let text: String = row.get(0)?;
            snapshot.push(
                serde_json::from_str::<Json>(&text)
                    .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?,
            );
        }
        snapshots.insert(table.clone(), Json::Array(snapshot));
    }
    Ok(Json::Object(snapshots))
}

fn restore_tables(connection: &Connection, snapshots: &Json) -> Result<()> {
    for (table, rows) in snapshots.as_object().ok_or_else(|| {
        Error::new(
            ErrorCode::SchemaMismatch,
            "table snapshot must be an object",
        )
    })? {
        let mut schema = connection.prepare(&format!("DESCRIBE {}", quote_identifier(table)))?;
        let columns: Vec<(String, String)> = schema
            .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
            .collect::<std::result::Result<_, _>>()?;
        let delete = format!("DELETE FROM {}", quote_identifier(table));
        connection.execute(&delete, [])?;
        for row in rows.as_array().ok_or_else(|| {
            Error::new(
                ErrorCode::SchemaMismatch,
                "table snapshot rows must be an array",
            )
        })? {
            let expressions = columns
                .iter()
                .map(|(name, _)| sql_literal(row.get(name).unwrap_or(&Json::Null)))
                .collect::<Vec<_>>()
                .join(", ");
            let sql = format!(
                "INSERT INTO {} VALUES ({expressions})",
                quote_identifier(table)
            );
            connection.execute(&sql, [])?;
        }
    }
    Ok(())
}

#[derive(Debug, Clone)]
/// Options used to create or open a project database.
pub struct ProjectOptions {
    pub database_path: PathBuf,
    pub domain: String,
    pub create_if_missing: bool,
    pub read_only: bool,
}

#[derive(Debug, Clone)]
/// Metadata loaded from the `PROJECT` table.
pub struct ProjectInfo {
    pub domain: String,
    pub metadata: Json,
    pub schema_version: i32,
    pub framework_version: String,
    pub created_at: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ParameterType {
    String,
    Integer,
    Real,
    Boolean,
    Array,
    Object,
    Table,
}

impl ParameterType {
    fn as_str(&self) -> &'static str {
        match self {
            Self::String => "string",
            Self::Integer => "integer",
            Self::Real => "real",
            Self::Boolean => "boolean",
            Self::Array => "array",
            Self::Object => "object",
            Self::Table => "table",
        }
    }

    fn parse(value: &str) -> Result<Self> {
        match value {
            "string" => Ok(Self::String),
            "integer" => Ok(Self::Integer),
            "real" => Ok(Self::Real),
            "boolean" => Ok(Self::Boolean),
            "array" => Ok(Self::Array),
            "object" => Ok(Self::Object),
            "table" => Ok(Self::Table),
            _ => Err(Error::new(
                ErrorCode::InvalidArgument,
                format!("unknown parameter type: {value}"),
            )),
        }
    }
}

#[derive(Debug, Clone)]
pub struct TypeDescriptor {
    pub kind: ParameterType,
    pub items: Option<Box<TypeDescriptor>>,
    pub table_schema: Option<TableSchema>,
}

impl TypeDescriptor {
    pub fn scalar(kind: ParameterType) -> Self {
        Self {
            kind,
            items: None,
            table_schema: None,
        }
    }

    pub fn array(items: TypeDescriptor) -> Self {
        Self {
            kind: ParameterType::Array,
            items: Some(Box::new(items)),
            table_schema: None,
        }
    }

    pub fn to_json(&self) -> Json {
        let mut output = json!({"type": self.kind.as_str()});
        if let Some(items) = &self.items {
            output["items"] = items.to_json();
        }
        if let Some(schema) = &self.table_schema {
            output["columns"] = schema.to_json();
        }
        output
    }

    pub fn from_json(value: &Json) -> Result<Self> {
        let kind =
            ParameterType::parse(value.get("type").and_then(Value::as_str).ok_or_else(|| {
                Error::new(ErrorCode::InvalidArgument, "type descriptor requires type")
            })?)?;
        let items = if kind == ParameterType::Array {
            Some(Box::new(Self::from_json(value.get("items").ok_or_else(
                || Error::new(ErrorCode::InvalidArgument, "array type requires items"),
            )?)?))
        } else {
            None
        };
        let table_schema = if kind == ParameterType::Table && value.get("columns").is_some() {
            Some(TableSchema::from_json(value.get("columns").unwrap())?)
        } else {
            None
        };
        Ok(Self {
            kind,
            items,
            table_schema,
        })
    }

    pub fn validate(&self, value: &Json) -> Result<()> {
        match self.kind {
            ParameterType::String if value.is_string() => Ok(()),
            ParameterType::Integer if value.as_i64().is_some() => Ok(()),
            ParameterType::Real if value.is_number() => Ok(()),
            ParameterType::Boolean if value.is_boolean() => Ok(()),
            ParameterType::Object if value.is_object() => Ok(()),
            ParameterType::Array => {
                let items = self.items.as_ref().ok_or_else(|| {
                    Error::new(ErrorCode::WorkflowValidation, "array type requires items")
                })?;
                value
                    .as_array()
                    .ok_or_else(|| Error::new(ErrorCode::WorkflowValidation, "expected array"))?
                    .iter()
                    .try_for_each(|item| items.validate(item))
            }
            ParameterType::Table => Table::from_json(value)?.validate(self.table_schema.as_ref()),
            _ => Err(Error::new(
                ErrorCode::WorkflowValidation,
                format!("expected {}", self.kind.as_str()),
            )),
        }
    }
}

#[derive(Debug, Clone)]
pub struct TableColumnDefinition {
    pub name: String,
    pub description: String,
    pub kind: ParameterType,
    pub required: bool,
}

#[derive(Debug, Clone)]
pub struct TableSchema {
    pub columns: Vec<TableColumnDefinition>,
}

impl TableSchema {
    pub fn to_json(&self) -> Json {
        Json::Array(self.columns.iter().map(|column| json!({"name": column.name, "description": column.description, "type": column.kind.as_str(), "required": column.required})).collect())
    }

    pub fn from_json(value: &Json) -> Result<Self> {
        let columns = value
            .as_array()
            .ok_or_else(|| Error::new(ErrorCode::InvalidArgument, "table schema must be an array"))?
            .iter()
            .map(|item| {
                Ok(TableColumnDefinition {
                    name: item
                        .get("name")
                        .and_then(Value::as_str)
                        .ok_or_else(|| {
                            Error::new(ErrorCode::InvalidArgument, "table column requires name")
                        })?
                        .to_owned(),
                    description: item
                        .get("description")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_owned(),
                    kind: ParameterType::parse(
                        item.get("type").and_then(Value::as_str).ok_or_else(|| {
                            Error::new(ErrorCode::InvalidArgument, "table column requires type")
                        })?,
                    )?,
                    required: item
                        .get("required")
                        .and_then(Value::as_bool)
                        .unwrap_or(true),
                })
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(Self { columns })
    }
}

#[derive(Debug, Clone)]
pub struct Table {
    pub columns: Vec<TableColumn>,
}

#[derive(Debug, Clone)]
pub struct TableColumn {
    pub name: String,
    pub kind: ParameterType,
    pub values: Vec<Json>,
}

impl Table {
    pub fn from_json(value: &Json) -> Result<Self> {
        let columns = value
            .get("columns")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::new(ErrorCode::WorkflowValidation, "table requires columns"))?
            .iter()
            .map(|item| {
                Ok(TableColumn {
                    name: item
                        .get("name")
                        .and_then(Value::as_str)
                        .ok_or_else(|| {
                            Error::new(ErrorCode::WorkflowValidation, "table column requires name")
                        })?
                        .to_owned(),
                    kind: ParameterType::parse(
                        item.get("type").and_then(Value::as_str).ok_or_else(|| {
                            Error::new(ErrorCode::WorkflowValidation, "table column requires type")
                        })?,
                    )?,
                    values: item
                        .get("values")
                        .and_then(Value::as_array)
                        .ok_or_else(|| {
                            Error::new(
                                ErrorCode::WorkflowValidation,
                                "table column requires values",
                            )
                        })?
                        .clone(),
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let table = Self { columns };
        table.validate(None)?;
        Ok(table)
    }

    pub fn row_count(&self) -> usize {
        self.columns.first().map_or(0, |column| column.values.len())
    }

    pub fn validate(&self, schema: Option<&TableSchema>) -> Result<()> {
        let rows = self.row_count();
        let mut names = HashMap::new();
        for column in &self.columns {
            if names.insert(column.name.clone(), ()).is_some() {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    "duplicate table column",
                ));
            }
            if column.values.len() != rows {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    "table columns must have equal lengths",
                ));
            }
            if let Some(schema) = schema {
                let definition = schema
                    .columns
                    .iter()
                    .find(|definition| definition.name == column.name)
                    .ok_or_else(|| {
                        Error::new(ErrorCode::WorkflowValidation, "unexpected table column")
                    })?;
                if definition.kind != column.kind {
                    return Err(Error::new(
                        ErrorCode::WorkflowValidation,
                        "table column type mismatch",
                    ));
                }
            }
        }
        if let Some(schema) = schema {
            for definition in &schema.columns {
                if definition.required && !names.contains_key(&definition.name) {
                    return Err(Error::new(
                        ErrorCode::WorkflowValidation,
                        format!("missing table column: {}", definition.name),
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn to_json(&self) -> Json {
        json!({"columns": self.columns.iter().map(|column| json!({"name": column.name, "type": column.kind.as_str(), "values": column.values})).collect::<Vec<_>>()})
    }
}

#[derive(Debug, Clone)]
pub struct ParameterDefinition {
    pub name: String,
    pub description: String,
    pub kind: TypeDescriptor,
    pub default: Option<Json>,
    pub required: bool,
    pub example: Option<Json>,
}

#[derive(Debug, Clone, Default)]
pub struct ParameterSchema {
    pub definitions: Vec<ParameterDefinition>,
}

impl ParameterSchema {
    pub fn resolve(&self, values: &Json) -> Result<Json> {
        let input = values.as_object().ok_or_else(|| {
            Error::new(
                ErrorCode::WorkflowValidation,
                "parameters must be an object",
            )
        })?;
        let mut output = serde_json::Map::new();
        for definition in &self.definitions {
            if let Some(default) = &definition.default {
                output.insert(definition.name.clone(), default.clone());
            }
            if definition.required
                && definition.default.is_none()
                && !input.contains_key(&definition.name)
            {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    format!("missing parameter: {}", definition.name),
                ));
            }
        }
        for (name, value) in input {
            let definition = self
                .definitions
                .iter()
                .find(|definition| definition.name == *name)
                .ok_or_else(|| {
                    Error::new(
                        ErrorCode::WorkflowValidation,
                        format!("unknown parameter: {name}"),
                    )
                })?;
            definition.kind.validate(value)?;
            output.insert(name.clone(), value.clone());
        }
        Ok(Json::Object(output))
    }
}

pub type MethodExecutor = Box<dyn Fn(&mut Project, &Json) -> Result<Json> + Send + Sync>;
pub type MethodValidator = Box<dyn Fn(&Json) -> Result<()> + Send + Sync>;

pub struct Method {
    pub id: String,
    pub name: String,
    pub description: String,
    pub domain: String,
    pub parameters: ParameterSchema,
    pub cacheable: bool,
    pub writes: Vec<String>,
    pub required_methods: Vec<String>,
    pub single_occurrence: bool,
    pub implemented: bool,
    executor: MethodExecutor,
    validator: Option<MethodValidator>,
}

impl Method {
    pub fn new(
        id: impl Into<String>,
        name: impl Into<String>,
        description: impl Into<String>,
        domain: impl Into<String>,
        parameters: ParameterSchema,
        executor: MethodExecutor,
    ) -> Self {
        Self {
            id: id.into(),
            name: name.into(),
            description: description.into(),
            domain: domain.into(),
            parameters,
            cacheable: false,
            writes: Vec::new(),
            required_methods: Vec::new(),
            single_occurrence: false,
            implemented: true,
            executor,
            validator: None,
        }
    }

    pub fn with_validator(mut self, validator: MethodValidator) -> Self {
        self.validator = Some(validator);
        self
    }

    pub fn to_json(&self) -> Json {
        json!({
            "id": self.id,
            "name": self.name,
            "description": self.description,
            "domain": self.domain,
            "required_methods": self.required_methods,
            "single_occurrence": self.single_occurrence,
            "parameters": self.parameters.definitions.iter().map(|definition| json!({
                "name": definition.name,
                "description": definition.description,
                "type": definition.kind.to_json(),
                "default": definition.default,
                "required": definition.required
                ,"example": definition.example
            })).collect::<Vec<_>>(),
            "cacheable": self.cacheable,
            "writes": self.writes
        })
    }
    pub fn resolve(&self, values: &Json) -> Result<Json> {
        let resolved = self.parameters.resolve(values)?;
        if let Some(validator) = &self.validator {
            validator(&resolved)?;
        }
        Ok(resolved)
    }
    pub fn unimplemented(mut self) -> Self {
        self.implemented = false;
        self
    }
    pub fn run(&self, project: &mut Project, values: &Json) -> Result<Json> {
        (self.executor)(project, &self.resolve(values)?)
    }
}

#[derive(Default)]
pub struct MethodRegistry {
    methods: HashMap<String, Method>,
}

pub type OperationExecutor = Box<dyn Fn(&mut Project, &Json) -> Result<Json> + Send + Sync>;
pub type OperationValidator = Box<dyn Fn(&Json) -> Result<()> + Send + Sync>;

pub struct Operation {
    pub id: String,
    pub name: String,
    pub description: String,
    pub domain: String,
    pub parameters: ParameterSchema,
    executor: OperationExecutor,
    validator: Option<OperationValidator>,
}

impl Operation {
    pub fn new(
        id: impl Into<String>,
        name: impl Into<String>,
        description: impl Into<String>,
        domain: impl Into<String>,
        parameters: ParameterSchema,
        executor: OperationExecutor,
    ) -> Self {
        let mut parameters = parameters;
        for (name, description, example) in [(
            "database_path",
            "Filesystem path of the DuckDB project database.",
            "/data/project.duckdb",
        )] {
            if !parameters
                .definitions
                .iter()
                .any(|definition| definition.name == name)
            {
                parameters.definitions.insert(
                    0,
                    ParameterDefinition {
                        name: name.into(),
                        description: description.into(),
                        kind: TypeDescriptor::scalar(ParameterType::String),
                        default: None,
                        required: true,
                        example: Some(json!(example)),
                    },
                );
            }
        }
        Self {
            id: id.into(),
            name: name.into(),
            description: description.into(),
            domain: domain.into(),
            parameters,
            executor,
            validator: None,
        }
    }
    pub fn with_validator(mut self, validator: OperationValidator) -> Self {
        self.validator = Some(validator);
        self
    }
    pub fn to_json(&self) -> Json {
        json!({"id": self.id, "name": self.name, "description": self.description, "domain": self.domain, "parameters": self.parameters.definitions.iter().map(|d| json!({"name": d.name, "description": d.description, "type": d.kind.to_json(), "default": d.default, "required": d.required, "example": d.example})).collect::<Vec<_>>()})
    }
    pub fn run(&self, project: &mut Project, values: &Json) -> Result<Json> {
        let resolved = self.parameters.resolve(values)?;
        if let Some(validator) = &self.validator {
            validator(&resolved)?;
        }
        (self.executor)(project, &resolved)
    }
}

#[derive(Default)]
pub struct OperationRegistry {
    operations: HashMap<String, Operation>,
}

impl OperationRegistry {
    pub fn register(&mut self, operation: Operation) -> Result<()> {
        if self
            .operations
            .insert(operation.id.clone(), operation)
            .is_some()
        {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "duplicate operation",
            ));
        }
        Ok(())
    }
    pub fn get(&self, id: &str) -> Result<&Operation> {
        self.operations.get(id).ok_or_else(|| {
            Error::new(
                ErrorCode::InvalidArgument,
                format!("unknown operation: {id}"),
            )
        })
    }
    pub fn list(&self, domain: &str) -> Vec<Json> {
        self.operations
            .values()
            .filter(|o| domain.is_empty() || o.domain == domain)
            .map(Operation::to_json)
            .collect()
    }
}

impl MethodRegistry {
    pub fn register(&mut self, method: Method) -> Result<()> {
        if self.methods.insert(method.id.clone(), method).is_some() {
            return Err(Error::new(ErrorCode::InvalidArgument, "duplicate method"));
        }
        Ok(())
    }
    pub fn get(&self, id: &str) -> Result<&Method> {
        self.methods.get(id).ok_or_else(|| {
            Error::new(
                ErrorCode::WorkflowValidation,
                format!("unknown method: {id}"),
            )
        })
    }
    pub fn list(&self, domain: &str) -> Vec<Json> {
        self.methods
            .values()
            .filter(|method| domain.is_empty() || method.domain == domain)
            .map(Method::to_json)
            .collect()
    }
}

#[derive(Debug, Clone)]
pub struct WorkflowStep {
    pub method: String,
    pub parameters: Json,
    pub metadata: Option<Json>,
}

#[derive(Debug, Clone, Default)]
pub struct Workflow {
    pub name: String,
    pub version: i32,
    pub domain: String,
    pub steps: Vec<WorkflowStep>,
}

impl Workflow {
    pub fn to_json(&self) -> Json {
        json!(self
            .steps
            .iter()
            .map(|step| {
                let mut value = step
                    .metadata
                    .clone()
                    .unwrap_or_else(|| json!({"id": step.method}));
                value["parameters"] = step.parameters.clone();
                value
            })
            .collect::<Vec<_>>())
    }
    pub fn to_json_with_registry(&self, registry: &MethodRegistry) -> Result<Json> {
        Ok(json!({
            "name": self.name,
            "version": self.version,
            "domain": self.domain,
            "steps": self.steps.iter().map(|step| {
                let method = registry.get(&step.method)?;
                let mut value = step.metadata.clone().unwrap_or_else(|| method.to_json());
                value["parameters"] = step.parameters.clone();
                Ok(value)
            }).collect::<Result<Vec<_>>>()?,
        }))
    }
    pub fn from_json(value: &Json) -> Result<Self> {
        if let Some(items) = value.as_array() {
            return Ok(Self {
                steps: items
                    .iter()
                    .map(|item| {
                        Ok(WorkflowStep {
                            method: item
                                .get("method")
                                .or_else(|| item.get("id"))
                                .and_then(Value::as_str)
                                .ok_or_else(|| {
                                    Error::new(
                                        ErrorCode::WorkflowValidation,
                                        "step requires method",
                                    )
                                })?
                                .to_owned(),
                            parameters: item
                                .get("parameters")
                                .cloned()
                                .unwrap_or_else(|| json!({})),
                            metadata: Some(item.clone()),
                        })
                    })
                    .collect::<Result<Vec<_>>>()?,
                ..Self::default()
            });
        }
        let object = value.as_object().ok_or_else(|| {
            Error::new(ErrorCode::WorkflowValidation, "workflow must be an object")
        })?;
        let steps = object
            .get("steps")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::new(ErrorCode::WorkflowValidation, "workflow requires steps"))?
            .iter()
            .map(|item| {
                Ok(WorkflowStep {
                    method: item
                        .get("method")
                        .or_else(|| item.get("id"))
                        .and_then(Value::as_str)
                        .ok_or_else(|| {
                            Error::new(ErrorCode::WorkflowValidation, "step requires method")
                        })?
                        .to_owned(),
                    parameters: item.get("parameters").cloned().unwrap_or_else(|| json!({})),
                    metadata: None,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(Self {
            name: object
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            version: object.get("version").and_then(Value::as_i64).unwrap_or(1) as i32,
            domain: object
                .get("domain")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            steps,
        })
    }
    pub fn validate(&self, registry: &MethodRegistry) -> Result<()> {
        let mut prior = Vec::new();
        let mut counts = HashMap::new();
        for step in &self.steps {
            let method = registry.get(&step.method)?;
            if !method.implemented {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    format!("method is not implemented: {}", step.method),
                ));
            }
            if !self.domain.is_empty() && !method.domain.is_empty() && self.domain != method.domain
            {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    "workflow domain mismatch",
                ));
            }
            for required in &method.required_methods {
                if !prior.contains(required) {
                    return Err(Error::new(
                        ErrorCode::WorkflowValidation,
                        format!("required method is not earlier in workflow: {required}"),
                    ));
                }
            }
            let count = counts.entry(step.method.clone()).or_insert(0);
            *count += 1;
            if method.single_occurrence && *count > 1 {
                return Err(Error::new(
                    ErrorCode::WorkflowValidation,
                    format!("method occurs too many times: {}", step.method),
                ));
            }
            method.resolve(&step.parameters)?;
            prior.push(step.method.clone());
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct CacheEntry {
    pub name: String,
    pub description: String,
    pub hash: String,
    pub data: Vec<u8>,
    pub created_at: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct AuditEntry {
    pub operation_type: String,
    pub object_type: String,
    pub details: Json,
    pub created_at: String,
}

/// DuckDB-backed project and its workflow state.
pub struct Project {
    options: ProjectOptions,
    info: ProjectInfo,
}

/// Durable execution lifecycle operations scoped to one project.
pub struct WorkflowExecutionManager<'a> {
    project: &'a Project,
}

impl<'a> WorkflowExecutionManager<'a> {
    pub fn new(project: &'a Project) -> Self {
        Self { project }
    }

    pub fn create(&self, request: &Json) -> Result<Json> {
        if let Some(status) = self
            .project
            .query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1")?
            .as_array()
            .and_then(|rows| rows.first())
            .and_then(|row| row["status"].as_str())
        {
            if matches!(status, "queued" | "running" | "cancelling") {
                return Err(Error::new(
                    ErrorCode::InvalidArgument,
                    "an active workflow execution already owns this project",
                ));
            }
        }
        let launch_snapshot = self.project.get_workflow()?.to_json().to_string();
        let connection = self.project.connection()?;
        let revision = request
            .get("workflow_revision")
            .and_then(Value::as_i64)
            .unwrap_or(0);

        let progress = request
            .get("progress")
            .cloned()
            .unwrap_or_else(|| json!({}));
        connection.execute("DELETE FROM WORKFLOW_EXECUTION", [])?;
        connection.execute("INSERT INTO WORKFLOW_EXECUTION (domain_id, workflow_revision, launch_snapshot, status, progress) VALUES (?1, ?2, ?3, 'queued', ?4)", params![self.project.get_domain(), revision, launch_snapshot, progress.to_string()])?;
        drop(connection);
        self.current()
    }

    pub fn current(&self) -> Result<Json> {
        let mut rows = self
            .project
            .query_json("SELECT domain_id, workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION LIMIT 1")?;
        let row = rows
            .as_array_mut()
            .and_then(|rows| rows.pop())
            .ok_or_else(|| {
                Error::new(ErrorCode::InvalidArgument, "workflow execution not found")
            })?;
        Ok(row)
    }

    pub fn list(&self) -> Result<Json> {
        let rows = self
            .project
            .query_json("SELECT domain_id, workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION")?;
        Ok(rows)
    }

    pub fn transition(&self, state: ExecutionState) -> Result<Json> {
        let row = self
            .project
            .query_json("SELECT * FROM WORKFLOW_EXECUTION")?
            .as_array()
            .and_then(|rows| rows.first())
            .cloned()
            .ok_or_else(|| {
                Error::new(ErrorCode::InvalidArgument, "workflow execution not found")
            })?;
        let current = execution_state(row.get("status").and_then(Value::as_str).unwrap_or(""))?;
        if !valid_execution_transition(current, state) {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "invalid execution transition",
            ));
        }
        self.project.connection()?.execute("UPDATE WORKFLOW_EXECUTION SET status = ?1, completed_at = CASE WHEN ?1 IN ('completed', 'failed', 'cancelled', 'interrupted') THEN CURRENT_TIMESTAMP ELSE NULL END, updated_at = CURRENT_TIMESTAMP", params![execution_state_name(state)])?;
        self.current()
    }

    pub fn cancel(&self) -> Result<Json> {
        let current = execution_state(
            self.current()?
                .get("status")
                .and_then(Value::as_str)
                .unwrap_or(""),
        )?;
        match current {
            ExecutionState::Queued => self.transition(ExecutionState::Cancelled),
            ExecutionState::Running => self.transition(ExecutionState::Cancelling),
            _ => Err(Error::new(
                ErrorCode::InvalidArgument,
                "execution cannot be cancelled",
            )),
        }
    }

    pub fn scheduler_tick(&self, worker_id: &str) -> Result<Json> {
        if worker_id.is_empty() {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "worker_id must not be empty",
            ));
        }
        self.project.connection()?.execute(
            "UPDATE WORKFLOW_EXECUTION SET status = 'running', process_id = ?1, server_id = ?1, started_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'queued' AND (process_id IS NULL OR process_id = '')",
            params![worker_id],
        )?;
        let row = self.current()?;
        if row["status"] != "running" {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "stale or conflicting workflow worker claim",
            ));
        }
        let owner: String = self.project.connection()?.query_row(
            "SELECT process_id FROM WORKFLOW_EXECUTION LIMIT 1",
            [],
            |row| row.get(0),
        )?;
        if owner != worker_id {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "stale or conflicting workflow worker claim",
            ));
        }
        Ok(row)
    }

    pub fn release_worker(&self, worker_id: &str, state: ExecutionState) -> Result<Json> {
        if worker_id.is_empty()
            || !matches!(
                state,
                ExecutionState::Completed
                    | ExecutionState::Failed
                    | ExecutionState::Cancelled
                    | ExecutionState::Interrupted
            )
        {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "invalid workflow worker release",
            ));
        }
        let target = execution_state_name(state);
        self.project.connection()?.execute(
            "UPDATE WORKFLOW_EXECUTION SET status = ?1, completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE process_id = ?2 AND status IN ('running', 'cancelling')",
            params![target, worker_id],
        )?;
        let row = self.current()?;
        let owner: String = self.project.connection()?.query_row(
            "SELECT process_id FROM WORKFLOW_EXECUTION LIMIT 1",
            [],
            |row| row.get(0),
        )?;
        if row["status"] != target || owner != worker_id {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "stale workflow worker cannot release execution",
            ));
        }
        Ok(row)
    }

    pub fn recover_interrupted(&self) -> Result<usize> {
        let rows = self.list()?;
        let count = rows
            .as_array()
            .map(|rows| {
                rows.iter()
                    .filter(|row| row.get("status").and_then(Value::as_str) == Some("running"))
                    .count()
            })
            .unwrap_or(0);
        self.project.connection()?.execute("UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'", [])?;
        Ok(count)
    }
}

fn execution_state(value: &str) -> Result<ExecutionState> {
    match value {
        "queued" => Ok(ExecutionState::Queued),
        "running" => Ok(ExecutionState::Running),
        "cancelling" => Ok(ExecutionState::Cancelling),
        "cancelled" => Ok(ExecutionState::Cancelled),
        "completed" => Ok(ExecutionState::Completed),
        "failed" => Ok(ExecutionState::Failed),
        "interrupted" => Ok(ExecutionState::Interrupted),
        _ => Err(Error::new(
            ErrorCode::InvalidArgument,
            format!("unknown execution state: {value}"),
        )),
    }
}

fn execution_state_name(state: ExecutionState) -> &'static str {
    match state {
        ExecutionState::Queued => "queued",
        ExecutionState::Running => "running",
        ExecutionState::Cancelling => "cancelling",
        ExecutionState::Cancelled => "cancelled",
        ExecutionState::Completed => "completed",
        ExecutionState::Failed => "failed",
        ExecutionState::Interrupted => "interrupted",
    }
}

impl Project {
    /// Creates a new project database and initializes its schema.
    pub fn create(options: ProjectOptions) -> Result<Self> {
        if let Some(parent) = options.database_path.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)
                    .map_err(|error| Error::new(ErrorCode::DatabaseError, error.to_string()))?;
            }
        }
        let project = Self::initialize(options, true)?;
        project.audit("create", "project", json!({}))?;
        Ok(project)
    }

    /// Opens an existing project database.
    pub fn open(options: ProjectOptions) -> Result<Self> {
        if !options.database_path.exists() {
            if options.create_if_missing {
                return Self::create(options);
            }
            return Err(Error::new(
                ErrorCode::ProjectNotFound,
                "database does not exist",
            ));
        }
        Self::initialize(options, false)
    }

    fn initialize(options: ProjectOptions, creating: bool) -> Result<Self> {
        let mut project = Self {
            options,
            info: ProjectInfo {
                domain: String::new(),
                metadata: json!({}),
                schema_version: 1,
                framework_version: FRAMEWORK_VERSION.into(),
                created_at: String::new(),
            },
        };
        let connection = project.connection()?;
        let legacy_registry: i64 = connection.query_row(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'main' AND table_name = 'PROJECTS'",
            [],
            |row| row.get(0),
        )?;
        if legacy_registry != 0 {
            return Err(Error::new(
                ErrorCode::SchemaMismatch,
                "legacy PROJECTS registry is not supported; expected PROJECT",
            ));
        }
        if !project.options.read_only {
            connection.execute_batch(&format!("CREATE TABLE IF NOT EXISTS PROJECT (domain_id VARCHAR NOT NULL, metadata JSON, workflow JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, schema_version INTEGER NOT NULL DEFAULT 1, framework_version VARCHAR NOT NULL DEFAULT '{FRAMEWORK_VERSION}'); CREATE TABLE IF NOT EXISTS CACHE (name VARCHAR NOT NULL, description VARCHAR NOT NULL, hash VARCHAR NOT NULL, data BLOB NOT NULL, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(hash)); CREATE TABLE IF NOT EXISTS AUDIT_TRAIL (operation_type VARCHAR NOT NULL, object_type VARCHAR NOT NULL, operation_details JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP); CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION (domain_id VARCHAR NOT NULL DEFAULT '', workflow_revision INTEGER NOT NULL, launch_snapshot JSON NOT NULL DEFAULT '{{}}', status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{{}}', result_reference VARCHAR, process_id VARCHAR, server_id VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, error VARCHAR, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP); CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION_STEP (workflow_revision INTEGER NOT NULL, step_index INTEGER NOT NULL PRIMARY KEY, method VARCHAR NOT NULL, parameters JSON NOT NULL DEFAULT '{{}}', parameter_hash VARCHAR NOT NULL, cache_key VARCHAR NOT NULL, status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{{}}', result_reference VARCHAR, error_code VARCHAR, error_message VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP);"))?;
            let project_exists: i64 =
                connection.query_row("SELECT COUNT(*) FROM PROJECT", [], |row| row.get(0))?;
            if project_exists != 0 && creating {
                return Err(Error::new(
                    ErrorCode::ProjectAlreadyExists,
                    "database already contains a project",
                ));
            }
            if creating {
                connection.execute(
                    "INSERT INTO PROJECT (domain_id, metadata, workflow) VALUES (?1, '{}', '[]')",
                    params![project.options.domain],
                )?;
            }
        }
        let project_count: i64 =
            connection.query_row("SELECT COUNT(*) FROM PROJECT", [], |row| row.get(0))?;
        if !creating && project_count != 1 {
            return Err(Error::new(
                ErrorCode::SchemaMismatch,
                "project database must contain exactly one PROJECT row",
            ));
        }
        let row = connection.query_row("SELECT COALESCE(domain_id, ''), COALESCE(metadata, '{}'), schema_version, framework_version, CAST(created_at AS VARCHAR) FROM PROJECT LIMIT 1", [], |row| Ok(ProjectInfo { domain: row.get(0)?, metadata: serde_json::from_str(&row.get::<_, String>(1)?).unwrap_or_else(|_| json!({})), schema_version: row.get(2)?, framework_version: row.get(3)?, created_at: row.get(4)? }))?;
        project.info = row;
        if !project.options.domain.is_empty() && project.info.domain != project.options.domain {
            return Err(Error::new(
                ErrorCode::SchemaMismatch,
                "project domain mismatch",
            ));
        }
        if !project.options.read_only && !creating {
            connection.execute(
                "UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'",
                [],
            )?;
        }
        Ok(project)
    }

    fn connection(&self) -> Result<Connection> {
        let home_directory = self
            .options
            .database_path
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .to_path_buf();
        let extension_directory = home_directory.join(".streamfind-duckdb-extensions").join(
            self.options
                .database_path
                .file_name()
                .unwrap_or_else(|| std::ffi::OsStr::new("default")),
        );
        fs::create_dir_all(&extension_directory)
            .map_err(|error| Error::new(ErrorCode::DatabaseError, error.to_string()))?;
        let config = Config::default()
            .with(
                // DuckDB resolves the autoload cache and ~ expansion against
                // the home directory; MCP subprocesses inherit a filtered env
                // (no HOME), so pin it to the project directory to keep the
                // extension autoload working everywhere.
                "home_directory",
                home_directory.to_string_lossy().as_ref(),
            )?
            .with(
                "extension_directory",
                extension_directory.to_string_lossy().as_ref(),
            )?;
        Ok(Connection::open_with_flags(
            &self.options.database_path,
            config,
        )?)
    }

    pub fn execute_sql(&self, sql: &str) -> Result<()> {
        if self.options.read_only {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "project is read-only",
            ));
        }
        self.connection()?.execute_batch(sql)?;
        Ok(())
    }

    pub fn query_json(&self, sql: &str) -> Result<Json> {
        let connection = self.connection()?;
        let mut metadata_statement = connection.prepare(sql)?;
        let _ = metadata_statement.query([])?;
        let columns = metadata_statement.column_count();
        let names: Vec<String> = (0..columns)
            .map(|i| {
                metadata_statement
                    .column_name(i)
                    .map(String::clone)
                    .unwrap_or_default()
            })
            .collect();
        let mut statement = connection.prepare(sql)?;
        let mut rows = statement.query([])?;
        let mut output = Vec::new();
        while let Some(row) = rows.next()? {
            let mut object = serde_json::Map::new();
            for (i, name) in names.iter().enumerate() {
                object.insert(name.clone(), duck_value_to_json(row.get(i)?));
            }
            output.push(Json::Object(object));
        }
        Ok(Json::Array(output))
    }

    pub fn info(&self) -> &ProjectInfo {
        &self.info
    }
    /// Returns a copy of the project metadata.
    pub fn get_metadata(&self) -> Json {
        self.info.metadata.clone()
    }
    pub fn get_database_path(&self) -> &Path {
        &self.options.database_path
    }
    pub fn get_domain(&self) -> String {
        self.info.domain.clone()
    }
    pub fn validate(&self) -> Result<()> {
        let tables = self.list_tables()?;
        let expected_domain_tables = crate::catalogue::table_manifest(&self.info.domain)
            .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?;
        for required in [
            "PROJECT",
            "CACHE",
            "AUDIT_TRAIL",
            "WORKFLOW_EXECUTION",
            "WORKFLOW_EXECUTION_STEP",
        ] {
            if !tables.iter().any(|table| table == required) {
                return Err(Error::new(
                    ErrorCode::SchemaMismatch,
                    format!("missing required table: {required}"),
                ));
            }
        }
        for (required, _) in expected_domain_tables {
            if !tables.iter().any(|table| table == &required) {
                return Err(Error::new(
                    ErrorCode::SchemaMismatch,
                    format!("missing expected domain table: {required}"),
                ));
            }
        }
        self.get_workflow()?;
        Ok(())
    }
    pub fn list_tables(&self) -> Result<Vec<String>> {
        let connection = self.connection()?;
        let mut statement = connection.prepare("SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' ORDER BY table_name")?;
        Ok(statement
            .query_map([], |row| row.get(0))?
            .collect::<std::result::Result<Vec<String>, _>>()?)
    }
    pub fn set_metadata(&mut self, metadata: Json) -> Result<()> {
        if !metadata.is_object() {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "project metadata must be an object",
            ));
        }
        if metadata
            .as_object()
            .unwrap()
            .values()
            .any(|value| value.is_object() || value.is_array())
        {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "project metadata values must be scalar",
            ));
        }
        self.connection()?.execute(
            "UPDATE PROJECT SET metadata = ?1, updated_at = CURRENT_TIMESTAMP",
            params![metadata.to_string()],
        )?;
        self.info.metadata = metadata;
        Ok(())
    }
    pub fn get_workflow(&self) -> Result<Workflow> {
        let text: String =
            self.connection()?
                .query_row("SELECT workflow FROM PROJECT", [], |row| row.get(0))?;
        Workflow::from_json(
            &serde_json::from_str(&text)
                .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?,
        )
    }
    pub fn set_workflow(&mut self, workflow: Workflow, registry: &MethodRegistry) -> Result<()> {
        workflow.validate(registry)?;
        let execution = self.query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1")?;
        let status = execution
            .as_array()
            .and_then(|rows| rows.first())
            .and_then(|row| row["status"].as_str())
            .unwrap_or_default();
        if matches!(status, "queued" | "running" | "cancelling") {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "workflow mutation is blocked while an execution is active",
            ));
        }
        let workflow_json = workflow.to_json_with_registry(registry)?;
        self.connection()?.execute(
            "UPDATE PROJECT SET workflow = ?1, updated_at = CURRENT_TIMESTAMP",
            params![workflow_json.to_string()],
        )?;
        self.audit("update", "workflow", workflow_json)
    }
    pub fn copy(&self, options: ProjectOptions) -> Result<Self> {
        let workflow = self.get_workflow()?.to_json();
        let mut destination_options = options;
        destination_options.domain = self.info.domain.clone();
        let mut destination = Self::create(destination_options)?;
        destination.set_metadata(self.info.metadata.clone())?;
        destination.connection()?.execute(
            "UPDATE PROJECT SET workflow = ?1, updated_at = CURRENT_TIMESTAMP",
            params![workflow.to_string()],
        )?;
        for entry in self.get_cache()? {
            let value = serde_json::from_slice(&entry.data)
                .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?;
            destination.set_cache(&entry.name, &entry.description, &entry.hash, &value)?;
        }
        Ok(destination)
    }
    pub fn get_cache(&self) -> Result<Vec<CacheEntry>> {
        let connection = self.connection()?;
        let mut statement = connection.prepare("SELECT name, description, hash, data, CAST(created_at AS VARCHAR) FROM CACHE ORDER BY created_at DESC")?;
        let rows = statement
            .query_map([], |row| {
                Ok(CacheEntry {
                    name: row.get(0)?,
                    description: row.get(1)?,
                    hash: row.get(2)?,
                    data: row.get(3)?,
                    created_at: row.get(4)?,
                })
            })?
            .collect::<std::result::Result<Vec<_>, _>>()?;
        Ok(rows)
    }
    pub fn get_cache_size(&self) -> Result<usize> {
        Ok(self.get_cache()?.len())
    }
    pub fn get_cache_entry(&self, hash: &str) -> Result<Option<CacheEntry>> {
        Ok(self.connection()?.query_row("SELECT name, description, hash, data, CAST(created_at AS VARCHAR) FROM CACHE WHERE hash = ?1", params![hash], |row| Ok(CacheEntry { name: row.get(0)?, description: row.get(1)?, hash: row.get(2)?, data: row.get(3)?, created_at: row.get(4)? })).optional()?)
    }
    pub fn set_cache(
        &mut self,
        name: &str,
        description: &str,
        hash: &str,
        value: &Json,
    ) -> Result<()> {
        self.connection()?.execute("INSERT INTO CACHE (name, description, hash, data) VALUES (?1, ?2, ?3, ?4) ON CONFLICT(hash) DO UPDATE SET name = excluded.name, description = excluded.description, data = excluded.data", params![name, description, hash, value.to_string().into_bytes()])?;
        Ok(())
    }
    pub fn delete_cache(&mut self) -> Result<()> {
        self.connection()?.execute("DELETE FROM CACHE", [])?;
        self.audit("delete", "cache", json!({}))
    }
    pub fn get_audit_trail(&self) -> Result<Vec<AuditEntry>> {
        let connection = self.connection()?;
        let mut statement = connection.prepare("SELECT operation_type, object_type, COALESCE(operation_details, '{}'), CAST(created_at AS VARCHAR) FROM AUDIT_TRAIL ORDER BY created_at ASC")?;
        let rows = statement
            .query_map([], |row| {
                Ok(AuditEntry {
                    operation_type: row.get(0)?,
                    object_type: row.get(1)?,
                    details: serde_json::from_str(&row.get::<_, String>(2)?)
                        .unwrap_or_else(|_| json!({})),
                    created_at: row.get(3)?,
                })
            })?
            .collect::<std::result::Result<Vec<_>, _>>()?;
        Ok(rows)
    }
    pub fn get_workflow_execution(&self) -> Result<Json> {
        self.query_json("SELECT workflow_revision, step_index, method, parameter_hash, status, started_at, completed_at, error_message AS error, cache_key FROM WORKFLOW_EXECUTION_STEP ORDER BY workflow_revision, step_index")
    }
    fn record_execution(
        &self,
        revision: i32,
        index: usize,
        method: &str,
        parameter_hash: &str,
        status: &str,
        cache_key: &str,
        launch_snapshot: &str,
        error: Option<&str>,
    ) -> Result<()> {
        let has_parent = !self
            .query_json("SELECT process_id FROM WORKFLOW_EXECUTION LIMIT 1")?
            .as_array()
            .is_none_or(Vec::is_empty);
        if !has_parent {
            self.connection()?.execute(
                "INSERT INTO WORKFLOW_EXECUTION (workflow_revision, launch_snapshot, status, error) VALUES (?1, ?2, ?3, ?4)",
                params![revision, launch_snapshot, status, error],
            )?;
        } else {
            self.connection()?.execute(
                "UPDATE WORKFLOW_EXECUTION SET workflow_revision = ?1, launch_snapshot = ?2, status = CASE WHEN process_id IS NULL OR process_id = '' THEN ?3 ELSE status END, error = ?4, updated_at = CURRENT_TIMESTAMP",
                params![revision, launch_snapshot, status, error],
            )?;
        }
        self.connection()?.execute("INSERT INTO WORKFLOW_EXECUTION_STEP (workflow_revision, step_index, method, parameters, parameter_hash, cache_key, status) VALUES (?1, ?2, ?3, '{}', ?4, ?5, ?6) ON CONFLICT(step_index) DO UPDATE SET workflow_revision = excluded.workflow_revision, method = excluded.method, parameter_hash = excluded.parameter_hash, cache_key = excluded.cache_key, status = excluded.status", params![revision, index as i64, method, parameter_hash, cache_key, status])?;
        Ok(())
    }
    pub fn run_method(
        &mut self,
        method_id: &str,
        parameters: &Json,
        registry: &MethodRegistry,
    ) -> Result<Json> {
        let method = registry.get(method_id)?;
        let mut workflow = self.get_workflow()?;
        let resolved = method.resolve(parameters)?;
        workflow.steps.push(WorkflowStep {
            method: method_id.to_owned(),
            parameters: resolved.clone(),
            metadata: None,
        });
        self.set_workflow(workflow, registry)?;
        let workflow = self.get_workflow()?;
        workflow.validate(registry)?;
        let launch_snapshot = workflow.to_json_with_registry(registry)?.to_string();
        let index = workflow.steps.len() - 1;
        let preceding_step_matches = if index == 0 {
            true
        } else {
            let parent_completed = self
                .query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1")?
                .as_array()
                .is_some_and(|rows| rows.first().is_some_and(|row| row["status"] == "completed"));
            if !parent_completed {
                false
            } else {
                self.get_workflow_execution()?
                    .as_array()
                    .is_some_and(|rows| {
                        rows.iter().any(|row| {
                            row["workflow_revision"] == workflow.version
                                && row["step_index"] == index - 1
                                && row["method"] == workflow.steps[index - 1].method
                                && row["status"] == "completed"
                                && row["cache_key"]
                                    .as_str()
                                    .is_some_and(|value| !value.is_empty())
                        })
                    })
            }
        };
        if !preceding_step_matches {
            let execution = self.run_workflow(&workflow, registry, None, None)?;
            return execution
                .results
                .as_array()
                .and_then(|results| results.last())
                .cloned()
                .ok_or_else(|| Error::new(ErrorCode::WorkflowValidation, "workflow has no steps"));
        }
        if workflow.steps[index].method != method_id {
            return Err(Error::new(
                ErrorCode::WorkflowValidation,
                "method is not the next planned workflow step",
            ));
        }
        if workflow.steps[index].parameters != resolved {
            return Err(Error::new(
                ErrorCode::WorkflowValidation,
                "parameters do not match the planned workflow step",
            ));
        }
        let previous_hash = if index == 0 {
            "initial".into()
        } else {
            self.get_workflow_execution()?
                .as_array()
                .and_then(|rows| {
                    rows.iter().find(|row| {
                        row["workflow_revision"] == workflow.version
                            && row["step_index"] == index - 1
                            && row["status"] == "completed"
                    })
                })
                .and_then(|row| row["cache_key"].as_str())
                .unwrap_or("initial")
                .to_owned()
        };
        if index > 0 && previous_hash == "initial" {
            return Err(Error::new(
                ErrorCode::WorkflowValidation,
                "previous workflow step has not completed",
            ));
        }
        let parameter_hash = cache_key("parameters", method, &resolved);
        let key = cache_key(&previous_hash, method, &resolved);
        self.record_execution(
            workflow.version,
            index,
            method_id,
            &parameter_hash,
            "running",
            &key,
            &launch_snapshot,
            None,
        )?;
        self.audit(
            "start",
            "method",
            json!({"method": method_id, "parameters": resolved}),
        )?;
        let result = if method.cacheable {
            if let Some(entry) = self.get_cache_entry(&key)? {
                let payload: Json = serde_json::from_slice(&entry.data)
                    .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?;
                if !payload["result"].is_null() && payload["tables"].is_object() {
                    restore_tables(&self.connection()?, &payload["tables"])?;
                    self.record_execution(
                        workflow.version,
                        index,
                        method_id,
                        &parameter_hash,
                        "completed",
                        &key,
                        &launch_snapshot,
                        None,
                    )?;
                    payload["result"].clone()
                } else {
                    return Err(Error::new(
                        ErrorCode::SchemaMismatch,
                        "cache entry has no materialized tables",
                    ));
                }
            } else {
                let result = method.run(self, &resolved)?;
                let snapshots = snapshot_tables(&self.connection()?, &method.writes)?;
                self.set_cache(
                    &method.id,
                    "workflow result",
                    &key,
                    &json!({"result": result, "tables": snapshots}),
                )?;
                self.record_execution(
                    workflow.version,
                    index,
                    method_id,
                    &parameter_hash,
                    "completed",
                    &key,
                    &launch_snapshot,
                    None,
                )?;
                result
            }
        } else {
            let result = method.run(self, &resolved)?;
            self.record_execution(
                workflow.version,
                index,
                method_id,
                &parameter_hash,
                "completed",
                &key,
                &launch_snapshot,
                None,
            )?;
            result
        };
        self.audit("complete", "method", json!({"method": method_id}))?;
        Ok(result)
    }
    pub fn run_operation(
        &mut self,
        operation_id: &str,
        parameters: &Json,
        registry: &OperationRegistry,
    ) -> Result<Json> {
        let operation = registry.get(operation_id)?;
        let mut input = parameters.clone();
        input["database_path"] = json!(self.get_database_path().to_string_lossy());
        operation.run(self, &input)
    }
    pub fn close(self) {}
    pub fn run_worker(&mut self, worker_id: &str, registry: &MethodRegistry) -> Result<Json> {
        WorkflowExecutionManager::new(self).scheduler_tick(worker_id)?;
        let workflow = self.get_workflow()?;
        let cancellation = CancellationToken::default();
        let result = match self.run_workflow(&workflow, registry, Some(&cancellation), None) {
            Ok(result) => result,
            Err(error) => {
                let _ = self.connection()?.execute(
                    "UPDATE WORKFLOW_EXECUTION SET error = ?1, updated_at = CURRENT_TIMESTAMP",
                    params![error.to_string()],
                );
                let _ = WorkflowExecutionManager::new(self)
                    .release_worker(worker_id, ExecutionState::Failed);
                return Err(error);
            }
        };
        WorkflowExecutionManager::new(self).release_worker(
            worker_id,
            if result.cancelled {
                ExecutionState::Cancelled
            } else {
                ExecutionState::Completed
            },
        )
    }
    pub fn run_workflow(
        &mut self,
        workflow: &Workflow,
        registry: &MethodRegistry,
        cancellation: Option<&CancellationToken>,
        progress: Option<&dyn Fn(&ProgressEvent)>,
    ) -> Result<ExecutionResult> {
        workflow.validate(registry)?;
        let launch_snapshot = workflow.to_json_with_registry(registry)?.to_string();
        self.connection()?
            .execute("DELETE FROM WORKFLOW_EXECUTION_STEP", [])?;
        let mut results = Vec::new();
        let mut previous_hash = "initial".to_string();
        for (index, step) in workflow.steps.iter().enumerate() {
            if cancellation.is_some_and(CancellationToken::is_cancelled) {
                return Ok(ExecutionResult {
                    results: Json::Array(results),
                    cancelled: true,
                });
            }
            if cancellation.is_some() {
                let status: String = self.connection()?.query_row(
                    "SELECT status FROM WORKFLOW_EXECUTION LIMIT 1",
                    [],
                    |row| row.get(0),
                )?;
                if status == "cancelling" {
                    return Ok(ExecutionResult {
                        results: Json::Array(results),
                        cancelled: true,
                    });
                }
            }
            let method = registry.get(&step.method)?;
            let parameters = method.resolve(&step.parameters)?;
            let key = cache_key(&previous_hash, method, &parameters);
            let parameter_hash = cache_key("parameters", method, &parameters);
            self.record_execution(
                workflow.version,
                index,
                &method.id,
                &parameter_hash,
                "pending",
                &key,
                &launch_snapshot,
                None,
            )?;
            if method.cacheable {
                if let Some(entry) = self.get_cache_entry(&key)? {
                    let payload: Json = serde_json::from_slice(&entry.data).map_err(|error| {
                        Error::new(ErrorCode::SchemaMismatch, error.to_string())
                    })?;
                    if payload.get("result").is_some()
                        && payload.get("tables").is_some_and(Json::is_object)
                    {
                        restore_tables(&self.connection()?, &payload["tables"])?;
                        results.push(payload["result"].clone());
                        self.audit(
                            "cache_hit",
                            "workflow_step",
                            json!({"method": method.id, "cache_key": key}),
                        )?;
                        self.record_execution(
                            workflow.version,
                            index,
                            &method.id,
                            &parameter_hash,
                            "completed",
                            &key,
                            &launch_snapshot,
                            None,
                        )?;
                        previous_hash = key;
                        self.connection()?.execute(
                            "UPDATE WORKFLOW_EXECUTION SET progress = ?1, updated_at = CURRENT_TIMESTAMP",
                            params![json!({"completed": index + 1, "total": workflow.steps.len(), "current_step": index}).to_string()],
                        )?;
                        continue;
                    }
                }
                self.audit(
                    "cache_miss",
                    "workflow_step",
                    json!({"method": method.id, "cache_key": key}),
                )?;
            }
            self.audit("start", "workflow_step", json!({"method": method.id}))?;
            self.record_execution(
                workflow.version,
                index,
                &method.id,
                &parameter_hash,
                "running",
                &key,
                &launch_snapshot,
                None,
            )?;
            let result = match method.run(self, &parameters) {
                Ok(result) => result,
                Err(error) => {
                    self.record_execution(
                        workflow.version,
                        index,
                        &method.id,
                        &parameter_hash,
                        "failed",
                        &key,
                        &launch_snapshot,
                        Some(&error.to_string()),
                    )?;
                    return Err(error);
                }
            };
            if method.cacheable {
                let snapshots = snapshot_tables(&self.connection()?, &method.writes)?;
                self.set_cache(
                    &method.id,
                    "workflow result",
                    &key,
                    &json!({"result": result.clone(), "tables": snapshots}),
                )?;
            }
            results.push(result);
            self.record_execution(
                workflow.version,
                index,
                &method.id,
                &parameter_hash,
                "completed",
                &key,
                &launch_snapshot,
                None,
            )?;
            self.connection()?.execute(
                "UPDATE WORKFLOW_EXECUTION SET progress = ?1, updated_at = CURRENT_TIMESTAMP",
                params![json!({"completed": index + 1, "total": workflow.steps.len(), "current_step": index}).to_string()],
            )?;
            previous_hash = key.clone();
            if let Some(progress) = progress {
                progress(&ProgressEvent {
                    operation: "workflow".into(),
                    completed: index + 1,
                    total: workflow.steps.len(),
                });
            }
            self.audit(
                "complete",
                "workflow_step",
                json!({"method": method.id, "cache_key": key}),
            )?;
        }
        Ok(ExecutionResult {
            results: Json::Array(results),
            cancelled: false,
        })
    }
    fn audit(&self, operation: &str, object: &str, details: Json) -> Result<()> {
        if self.options.read_only {
            return Ok(());
        }
        self.connection()?.execute("INSERT INTO AUDIT_TRAIL (operation_type, object_type, operation_details) VALUES (?1, ?2, ?3)", params![operation, object, details.to_string()])?;
        Ok(())
    }
}
