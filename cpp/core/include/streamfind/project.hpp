#pragma once

#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <atomic>
#include <functional>
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
    bool cacheable{false};
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
    std::string id, name, description, domain;
    ParameterSchema parameters;
};
using OperationExecutor = std::function<Json(Project &, const Json &)>;
using OperationValidator = std::function<void(const Json &)>;

class STREAMFIND_CORE_API Operation {
public:
    Operation(OperationDefinition definition, OperationExecutor executor = {},
              OperationValidator validator = {});
    const OperationDefinition &definition() const noexcept;
    Json to_json() const;
    Json resolve_parameters(const Json &value) const;
    Json run(Project &project, const Json &value) const;
private:
    OperationDefinition definition_;
    OperationExecutor executor_;
    OperationValidator validator_;
};

class STREAMFIND_CORE_API OperationRegistry {
public:
    void register_operation(Operation operation);
    const Operation *find(const std::string &id) const noexcept;
    std::vector<OperationDefinition> list(const std::string &domain = {}) const;
private:
    std::vector<Operation> operations_;
};

/** @brief Return the process-wide default method registry. */
STREAMFIND_CORE_API MethodRegistry &methods();

/** @brief One ordered method invocation in a workflow. */
struct STREAMFIND_CORE_API WorkflowStep {
    /// Registered method identifier.
    std::string method;
    /// Values passed to that method.
    ParameterValues parameters;

    /** @brief Export the step method id and values as JSON. */
    Json to_json() const;
    /** @brief Parse a workflow step from JSON. */
    static WorkflowStep from_json(const Json &value);
};

/** @brief Ordered, versioned method workflow owned by a Project. */
class STREAMFIND_CORE_API Workflow {
public:
    /// Display name of the workflow.
    std::string name;
    /// Incremented whenever a Project stores a new workflow definition.
    int version{1};
    /// Domain this workflow belongs to.
    std::string domain;
    /// Ordered method invocations.
    std::vector<WorkflowStep> steps;

    /** @brief Validate method ids, ordering, domains, occurrences, and values. */
    void validate(const MethodRegistry &registry) const;
    /** @brief Validate with table availability from the target project. */
    void validate(const MethodRegistry &registry,
                  const std::function<bool(std::string_view)> &has_table) const;
    /** @brief Export the workflow definition as JSON. */
    Json to_json() const;
    /** @brief Export ordered method metadata with configured parameter values. */
    Json to_json(const MethodRegistry &registry) const;
    /** @brief Parse a workflow object or legacy ordered array from JSON. */
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

/** @brief Cooperative cancellation state for long-running operations. */
class STREAMFIND_CORE_API CancellationToken {
public:
    /** @brief Request cancellation. */
    void cancel() noexcept;
    /** @brief Return whether cancellation was requested. */
    bool is_cancelled() const noexcept;
private:
    std::atomic<bool> cancelled_{false};
};

/** @brief Progress snapshot emitted during execution. */
struct STREAMFIND_CORE_API ProgressEvent {
    std::string operation;
    std::size_t completed{0};
    std::size_t total{0};
};

using ProgressCallback = std::function<void(const ProgressEvent &)>;

/** @brief Stable result envelope for workflow execution. */
struct STREAMFIND_CORE_API ExecutionResult {
    Json results{Json::array()};
    bool cancelled{false};
    Json to_json() const;
};

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
    /// Domain assigned once when a project is created.
    std::string domain;
    /// Project-owned metadata initialized on creation.
    Json metadata{Json::object()};
};

/** @brief Persisted identity and metadata for an open Project. */
struct STREAMFIND_CORE_API ProjectInfo {
    /// Domain selected for the project.
    std::string domain;
    /// Project-owned metadata.
    Json metadata{Json::object()};
    int schema_version{1};
    std::string framework_version;
    std::string created_at;
};

/** @brief One serialized entry in the Project CACHE table. */
struct STREAMFIND_CORE_API CacheEntry {
    /// Cache operation/name label.
    std::string name;
    /// Human-readable cache description.
    std::string description;
    /// Deterministic cache key.
    std::string hash;
    std::vector<std::uint8_t> data;
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
    /** @brief Return the project domain. */
    std::string get_domain() const;
    /** @brief Validate the project schema and persisted row state. */
    void validate() const;
    Workflow get_workflow() const;
    /** @brief Persist a workflow using the supplied method registry. */
    void set_workflow(Workflow workflow, const MethodRegistry &registry = methods());
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

    /** @brief Return all cache entries for this project. */
    std::vector<CacheEntry> get_cache() const;
    /** @brief Return the number of cache entries for this project. */
    std::size_t get_cache_size() const;
    /** @brief Find a cache entry by deterministic hash. */
    std::optional<CacheEntry> get_cache_entry(const std::string &hash) const;
    /** @brief Insert or replace a JSON value in the project cache. */
    void set_cache(std::string name, std::string description,
                   std::string hash, const Json &value);
    /** @brief Delete all cache entries for this project. */
    void delete_cache();
    /** @brief Return processing and cache audit events in time order. */
    std::vector<AuditEntry> get_audit_trail() const;
    /** @brief Return persisted execution rows for every workflow step. */
    Json get_workflow_execution() const;

    /** @brief Execute the persisted workflow using a method registry. */
    ExecutionResult run_workflow(const MethodRegistry &registry = methods(),
                                 CancellationToken *cancellation = nullptr,
                                 ProgressCallback progress = {});
    /** @brief Claim, execute, and release one externally-triggered worker run. */
    Json run_worker(const std::string &worker_id,
                    const MethodRegistry &registry = methods());
    /** @brief Execute one registered method with supplied parameters. */
    Json run_method(const std::string &method_id, const Json &parameters,
                    const MethodRegistry &registry = methods());
    Json run_operation(const std::string &operation_id, const Json &parameters,
                       const OperationRegistry &registry) const;
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
