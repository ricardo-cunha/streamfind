#pragma once

#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"
#include "streamfind/sdk/method_execution_context.hpp"
/** @brief Public streamfind core Project, workflow, and method API. */
namespace streamfind {

/** @brief JSON value used by the public core API. */
using Json = nlohmann::json;

class Project;

/** @brief Primitive and container types supported by method parameters. */
enum class STREAMFIND_CORE_API ParameterType {
    string,
    integer,
    real,
    boolean,
    array,
    object,
    table,
};

/** @brief Documentation and validation rules for one table column. */
struct STREAMFIND_CORE_API TableColumnDefinition {
    /// Stable column name used in JSON and table lookups.
    std::string name;
    /// Human-readable column documentation.
    std::string description;
    /// Scalar type stored in the column.
    ParameterType type{ParameterType::string};
    /// Whether the column must be present in a value.
    bool required{true};

    /** @brief Export this column definition as JSON. */
    Json to_json() const;
    /** @brief Construct a column definition from JSON. */
    static TableColumnDefinition from_json(const Json &value);
};

/** @brief Ordered schema for a typed table parameter. */
struct STREAMFIND_CORE_API TableSchema {
    /// Columns in their documented and serialized order.
    std::vector<TableColumnDefinition> columns;

    /** @brief Export this table schema as JSON. */
    Json to_json() const;
    /** @brief Construct a table schema from JSON. */
    static TableSchema from_json(const Json &value);
};

/** @brief One typed, column-oriented table value. */
struct STREAMFIND_CORE_API TableColumn {
    /// Column name.
    std::string name;
    /// Type of every value in the column.
    ParameterType type{ParameterType::string};
    /// Homogeneous values for this column.
    std::variant<std::vector<std::string>, std::vector<std::int64_t>,
                 std::vector<double>, std::vector<bool>> values;
};

/** @brief Columnar table value accepted by table-typed method parameters. */
struct STREAMFIND_CORE_API Table {
    /// Columns with equal row counts.
    std::vector<TableColumn> columns;

    /** @brief Return the number of rows, or zero for an empty table. */
    std::size_t row_count() const;
    /** @brief Validate column names, types, lengths, and an optional schema. */
    void validate(const std::optional<TableSchema> &schema = std::nullopt) const;
    /** @brief Export the table using the public columnar JSON format. */
    Json to_json() const;
    /** @brief Parse and validate a columnar JSON table. */
    static Table from_json(const Json &value);
};

/** @brief Recursive type description for scalar, array, and table values. */
struct STREAMFIND_CORE_API TypeDescriptor {
    /// The value kind described by this object.
    ParameterType kind{ParameterType::string};
    /// Element type for an array descriptor.
    std::shared_ptr<TypeDescriptor> items;
    /// Optional column schema for a table descriptor.
    std::optional<TableSchema> table_schema;

    /** @brief Export this type, including array items or table columns, as JSON. */
    Json to_json() const;
    /** @brief Parse a recursive type description from JSON. */
    static TypeDescriptor from_json(const Json &value);
    /** @brief Validate a JSON value against this type description. */
    void validate(const Json &value) const;
};

/** @brief Documentation, defaults, and validation metadata for one parameter. */
struct STREAMFIND_CORE_API ParameterDefinition {
    /// Stable parameter name.
    std::string name;
    /// Documentation shown to users and generated UI.
    std::string description;
    /// Recursive type and table/array shape information.
    TypeDescriptor type;
    /// Default value applied when the parameter is omitted.
    Json default_value{nullptr};
    /// Whether a value is required when no default exists.
    bool required{false};
    /// Representative value shown in generated tool documentation.
    Json example{nullptr};
    /// Machine-readable validation constraints.
    Json constraints{Json::object()};
    /// UI hints that do not affect execution semantics.
    Json ui{Json::object()};

    /** @brief Export this parameter definition as JSON. */
    Json to_json() const;
    /** @brief Construct a parameter definition from JSON. */
    static ParameterDefinition from_json(const Json &value);
};

/** @brief Ordered definitions for all parameters accepted by a method. */
struct STREAMFIND_CORE_API ParameterSchema {
    /// Parameters in the order used for documentation and UI rendering.
    std::vector<ParameterDefinition> definitions;

    /** @brief Export the schema as a JSON array. */
    Json to_json() const;
    /** @brief Construct a schema from a JSON array. */
    static ParameterSchema from_json(const Json &value);
    /** @brief Apply defaults and validate supplied values. */
    Json resolve_and_validate(const Json &values) const;
};

/** @brief Runtime parameter values without documentation metadata. */
struct STREAMFIND_CORE_API ParameterValues {
    /// Runtime values only; definitions live in MethodDefinition.
    Json values{Json::object()};

    /** @brief Export the runtime values as a JSON object. */
    Json to_json() const;
    /** @brief Parse runtime values from a JSON object. */
    static ParameterValues from_json(const Json &value);
};

/** @brief A table dependency activated by a boolean method parameter. */
struct STREAMFIND_CORE_API ConditionalRead {
    std::string table;
    std::string parameter;
    Json equals;
};

/** @brief Complete documented and executable description of a method. */
struct STREAMFIND_CORE_API MethodDefinition {
    /// Stable registry and persisted workflow identifier.
    std::string id;
    /// Short display name.
    std::string name;
    /// Human-readable method documentation.
    std::string description;
    std::string version{"1"};
    std::string domain;
    /// Tables that must exist before this method can execute.
    std::vector<std::string> reads;
    /// Additional tables required when the named boolean parameter is true.
    std::vector<ConditionalRead> conditional_reads;
    bool single_occurrence{false};
    std::string developer;
    std::string contact;
    std::string link;
    std::string doi;
    ParameterSchema parameters;

    /// Tables whose rows must be captured for cache materialization.
    std::vector<std::string> writes;
};

/** @brief Callback that executes a method against its owning Project. */
using MethodExecutor = std::function<Json(Project &, const Json &)>;
/** @brief Callback for method-specific and cross-parameter validation. */
using MethodValidator = std::function<void(const Json &)>;

/** @brief Executable method definition registered with a Project workflow. */
class STREAMFIND_CORE_API Method {
public:
    /** @brief Construct a method with execution and optional validation callbacks. */
    Method(MethodDefinition definition, MethodExecutor executor = {},
           MethodValidator validator = {},
           sdk::ContextMethodExecutor context_executor = {});

    /** @brief Return immutable method metadata. */
    const MethodDefinition &definition() const noexcept;
    /** @brief Export method metadata and its parameter schema as JSON. */
    Json to_json() const;
    /** @brief Parse method metadata without an executable callback. */
    static MethodDefinition definition_from_json(const Json &value);
    /** @brief Run the method-specific validator for supplied values. */
    void validate_parameters(const Json &value) const;
    /** @brief Apply defaults and validate values against the method schema. */
    Json resolve_parameters(const Json &value) const;
    /** @brief Execute the method against a Project. */
    Json run(Project &project, const Json &parameters,
             const sdk::ExecutionServices &services = {}) const;
    /** @brief Whether the method has an executable implementation. */
    bool implemented() const noexcept;

private:
    MethodDefinition definition_;
    MethodExecutor executor_;
    MethodValidator validator_;
    sdk::ContextMethodExecutor context_executor_;
};

/** @brief Registry of executable methods addressed by stable method id. */
class STREAMFIND_CORE_API MethodRegistry {
public:
    /** @brief Register a method; duplicate ids are rejected. */
    void register_method(Method method);
    /** @brief Find a method by id, or return nullptr. */
    const Method *find(const std::string &id) const noexcept;
    /** @brief Return metadata for registered methods, optionally filtered by domain. */
    std::vector<MethodDefinition> list(const std::string &domain = {}) const;

private:
    std::vector<Method> methods_;
};

struct STREAMFIND_CORE_API OperationDefinition {
    struct Port {
        std::string id;
        std::string semantic_contract;
        std::string cardinality{"one"};
        std::string data_kind;
        std::vector<std::string> representations;
        bool optional{false};

        Json to_json() const;
        static Port from_json(const Json &value);
    };

    std::string id, name, description, domain, version{"1"};
    bool project_entry{false};

    ParameterSchema parameters;
    std::vector<Port> input_ports;
    std::vector<Port> output_ports;
};
using WorkflowOperationExecutor = std::function<Json(
    Project &, const Json &, const std::string &, const Json &)>;
using OperationValidator = std::function<void(const Json &)>;

class STREAMFIND_CORE_API Operation {
public:
    Operation(OperationDefinition definition,
              WorkflowOperationExecutor executor = {},
              OperationValidator validator = {});
    const OperationDefinition &definition() const noexcept;
    Json to_json() const;
    Json resolve_parameters(const Json &value) const;
    Json run_workflow(Project &project, const Json &value,
                      const std::string &operation_instance,
                      const Json &inputs) const;
private:
    OperationDefinition definition_;
    OperationValidator validator_;
    WorkflowOperationExecutor executor_;
};

class STREAMFIND_CORE_API OperationRegistry {
public:
    void register_operation(Operation operation);
    const Operation *find(const std::string &id) const noexcept;
    std::vector<OperationDefinition> list(const std::string &domain = {}) const;
private:
    std::vector<Operation> operations_;
};

/** @brief One operation instance in a persisted workflow graph. */
struct STREAMFIND_CORE_API WorkflowOperation {
    std::string id;
    std::string operation;
    ParameterValues parameters;
    Json inputs{Json::object()};
    /// Optional canvas presentation coordinates; omitted for portable/API-created workflows.
    Json position{Json::object()};

    Json to_json() const;
    static WorkflowOperation from_json(const Json &value);
};

/** @brief One typed output-port to input-port workflow connection. */
struct STREAMFIND_CORE_API WorkflowConnection {
    std::string source_operation;
    std::string source_port;

    std::string target_operation;
    std::string target_port;

    Json to_json() const;
    static WorkflowConnection from_json(const Json &value);
};

/** @brief Versioned workflow graph owned by a Project. */
class STREAMFIND_CORE_API Workflow {
public:
    /// Version of the portable workflow-definition document format.
    int schema_version{1};
    /// Stable identity of the reusable workflow definition.
    std::string workflow_id;
    /// Display name of the workflow.
    std::string name;
    /// Incremented whenever a Project stores a new workflow definition.
    int version{1};
    /// Operation instances forming the backend execution graph.
    std::vector<WorkflowOperation> operations;
    /// Explicit typed-port dataflow connections.
    std::vector<WorkflowConnection> connections;

    /** @brief Validate operation instances and port bindings against the installed catalogue. */
    void validate(const OperationRegistry &registry) const;
    /** @brief Export the workflow definition as JSON. */
    Json to_json() const;

    /** @brief Parse a canonical workflow object from JSON. */
    static Workflow from_json(const Json &value);
};

/** @brief Categories raised by Project and workflow operations. */
enum class STREAMFIND_CORE_API ErrorCode {
    InvalidArgument,
    ProjectNotFound,
    ProjectAlreadyExists,
    SchemaMismatch,
    DatabaseError,
    WorkflowValidation,
    MethodExecution,
    ProjectClosed,
    Cancelled,
};

/** @brief Durable workflow execution lifecycle states. */
enum class STREAMFIND_CORE_API ExecutionState {
    queued,
    running,
    cancelling,
    cancelled,
    completed,
    failed,
    interrupted,
};

/** @brief Return whether a workflow execution lifecycle transition is allowed. */
STREAMFIND_CORE_API bool valid_execution_transition(ExecutionState from,
                                                     ExecutionState to) noexcept;


/** @brief Typed exception raised by the streamfind core API. */
class STREAMFIND_CORE_API Error : public std::runtime_error {
public:
    /** @brief Construct an error with a stable category and message. */
    Error(ErrorCode code, std::string message);
    /** @brief Return the stable error category. */
    ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

/** @brief Options used when creating or opening a Project database. */
struct STREAMFIND_CORE_API ProjectOptions {
    /// DuckDB file to create or open.
    std::filesystem::path database_path;
    /// Project-owned metadata initialized on creation.
    Json metadata{Json::object()};
};

/** @brief Persisted identity and metadata for an open Project. */
struct STREAMFIND_CORE_API ProjectInfo {
    /// Domains represented by persisted workflow operations.
    std::vector<std::string> domains;
    /// Project-owned metadata.
    Json metadata{Json::object()};
    int schema_version{1};
    std::string framework_version;
    std::string created_at;
};


/** @brief One processing event from the Project AUDIT_TRAIL table. */
struct STREAMFIND_CORE_API AuditEntry {
    /// Event operation, such as `start`, `complete`, or `cache_hit`.
    std::string operation_type;
    /// Audited object category.
    std::string object_type;
    /// Structured event details.
    Json details{Json::object()};
    std::string created_at;
};

/** @brief RAII handle for a DuckDB-backed streamfind Project. */
class STREAMFIND_CORE_API Project {
public:
    using OperationLogCallback = std::function<void(std::string_view)>;
    /** @internal Implementation state shared by the Project handle. */
    struct Impl;
    /** @internal Construct from initialized implementation state. */
    explicit Project(std::shared_ptr<Impl> impl);

    /** @brief Create a new Project database and project row. */
    static Project create(const ProjectOptions &options);
    /** @brief Open an existing Project database. */
    static Project open(const ProjectOptions &options);

    Project(Project &&) noexcept;
    Project &operator=(Project &&) noexcept;
    ~Project();
    Project(const Project &) = delete;
    Project &operator=(const Project &) = delete;

    /** @brief Return the current Project identity and metadata. */
    const ProjectInfo &info() const;
    /** @brief Return a copy of the project metadata. */
    Json get_metadata() const;
    const std::filesystem::path &get_database_path() const noexcept;

    /** @brief Replace project metadata. */
    void set_metadata(Json metadata);
    /** @brief Return domains represented by persisted workflow operations. */
    std::vector<std::string> get_domains() const;
    /** @brief Validate the project schema and persisted row state. */
    void validate() const;
    Workflow get_workflow() const;
    /** @brief Replace a workflow without revalidation, for copying an already validated graph. */
    void set_workflow(Workflow workflow);
    /** @brief Persist an operation workflow after validating it against installed operations. */
    void set_workflow(Workflow workflow, const OperationRegistry &registry);
    /** @brief Remove persisted workflow revisions older than the current revision. */
    void clear_workflow_history();
    /** @brief Remove cache manifests while retaining published artifacts. */
    void clear_artifact_cache();
    /** @brief Remove every published artifact and cache entry while retaining the workflow. */
    void clear_all_artifacts();
    /** @brief Copy this project to a new database. */
    Project copy(const ProjectOptions &options) const;
    /** @brief List tables visible in the project database. */
    std::vector<std::string> list_tables() const;
    /** @brief Execute domain-owned SQL using the Project database connection. */
    void execute_sql(const std::string &sql) const;
    /** @brief Execute a query and return rows as JSON objects keyed by column name. */
    Json query_json(const std::string &sql) const;

    /**
     * @brief Bulk-append rows into a table using a single DuckDB Appender session.
     *
     * Equivalent to the DuckDB Appender path used by the R bindings: all rows are
     * written through one `duckdb_appender` that is flushed (and closed) once, instead
     * of one `INSERT` per row. Each cell is an optional string; `std::nullopt` becomes
     * SQL NULL, otherwise the value is bound by the reflected DuckDB column type so
     * numeric columns stay numeric (DOUBLE/INTEGER/BOOLEAN) rather than becoming text.
     * Only the supplied `column_names` are appended; table columns not listed here are
     * filled with their DEFAULT value (or NULL).
     */
    void append_rows(const std::string &table_name,
                     const std::vector<std::string> &column_names,
                     const std::vector<std::vector<std::optional<std::string>>> &rows) const;


    /** @brief Return processing and cache audit events in time order. */
    std::vector<AuditEntry> get_audit_trail() const;
    /** @brief Return persisted execution rows for every workflow step. */
    Json get_workflow_execution() const;
    /** @brief Return immutable table and structured result artifacts published by workflows. */
    Json get_artifact_inventory() const;
    /** @brief Return the newest published artifact for each producer/output contract. */
    Json get_current_artifact_inventory() const;
    /** @brief Publish a structured JSON result for an operation output port. */
    std::string publish_result_artifact(const std::string &contract_id,
                                        const Json &payload,
                                        const std::string &producer_operation,
                                        const std::string &producer_instance,
                                        int workflow_revision = 0);
    /** @brief Resolve graph connections for one operation to published input artifacts. */
    Json resolve_workflow_inputs(const std::string &operation_id) const;
    Json resolve_workflow_inputs(const std::string &operation_id,
                                 const Json &current_artifacts) const;
    /** @brief Execute the persisted operation graph in topological order. */
    Json run_operation_graph(const OperationRegistry &registry);

    /** @brief Claim, execute, and release one externally-triggered worker run. */
    Json run_worker(const std::string &worker_id,
                    const OperationRegistry &registry);
    Json run_operation(const std::string &operation_id, const Json &parameters,
                       const OperationRegistry &registry,
                       const std::string &operation_instance = {},
                       const Json &provided_inputs = Json(nullptr));
    void set_operation_log_callback(OperationLogCallback callback);
    void log_operation(std::string_view message) const;
    /** @brief Mark the Project closed; subsequent operations fail. */
    void close() noexcept;

private:
    std::shared_ptr<Impl> impl_;
};

/** @brief Durable execution lifecycle operations scoped to one Project. */
class STREAMFIND_CORE_API WorkflowExecutionManager {
public:
    explicit WorkflowExecutionManager(Project &project) noexcept;
    Json create(const Json &request = Json::object());
    Json current() const;
    Json list() const;
    Json transition(ExecutionState state);
    Json cancel();
    Json scheduler_tick(const std::string &worker_id);
    Json release_worker(const std::string &worker_id, ExecutionState state);
    std::size_t recover_interrupted();

private:
    Project *project_;
};

}
