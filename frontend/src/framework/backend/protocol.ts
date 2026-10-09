export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

export type StreamFindEvent = {
  type: string;
  project?: string | JsonValue;
  operation_id?: string;
  execution_id?: string;
  timestamp?: string;
  event_id?: number;
  timestamp_ms?: number;
  payload?: JsonValue;
};

export type WorkflowEventHistoryResponse = {
  project: string;
  events: StreamFindEvent[];
  has_more: boolean;
  gap?: boolean;
  oldest_event_id?: number | null;
  latest_event_id?: number | null;
};

export type ServiceSession = {
  service: string;
  protocol_version: string;
  state: string;
  backend_version: string;
};

export type ProjectSession = {
  session_id: string;
  database_path: string;
  database_size_bytes: number;
  domains: string[];
  metadata: Record<string, JsonValue>;
  initialization?: ProjectInitialization;
};

export type ProjectInitialization = {
  required: boolean;
  state: 'not_required' | 'awaiting_input' | 'running' | 'completed' | 'failed';
  operation_id?: string;
  operation?: OperationCapability;
  operations?: OperationCapability[];
};

export type WorkflowState =
  'idle' | 'validated' | 'queued' | 'running' | 'cancelling' | 'completed' | 'failed' | 'cancelled';

export type WorkflowProgress = {
  completed: number;
  total: number;
  current_step: number;
};

export type WorkflowStateResponse = {
  session_id: string;
  state: WorkflowState;
  progress?: WorkflowProgress;
};

export type WorkflowValidationResponse = WorkflowStateResponse & { valid: boolean };

export type WorkflowPosition = { x: number; y: number };

export type WorkflowOperationDefinition = {
  id: string;
  operation: string;
  parameters: Record<string, JsonValue>;
  inputs?: Record<string, JsonValue>;
  position?: WorkflowPosition;
};

export type WorkflowConnectionDefinition = {
  source_operation: string;
  source_port: string;
  target_operation: string;
  target_port: string;
};

/** Portable workflow definition. Runtime execution data is persisted separately by the service. */
export type WorkflowDefinition = {
  schema_version: 1;
  workflow_id?: string;
  name?: string;
  version: number;
  metadata: WorkflowMetadata;
  operations: WorkflowOperationDefinition[];
  connections: WorkflowConnectionDefinition[];
};

export type WorkflowDemoMetadata = {
  id: string;
  name: string;
  description: string;
  use_case?: string;
  domain?: string;
  version?: number;
  workflow: WorkflowDefinition;
  [key: string]: JsonValue | WorkflowDefinition | undefined;
};

export type WorkflowMetadata = Record<string, JsonValue>;

export type WorkflowDiagnostic = { message: string };

export type WorkflowDefinitionResponse = {
  workflow: WorkflowDefinition;
  valid: boolean;
  diagnostics: WorkflowDiagnostic[];
};

/** JSON Schema subset used for parameters, table inputs, and result contracts. */
export type JsonSchema = {
  type?: string | string[];
  title?: string;
  description?: string;
  format?: string;
  enum?: JsonValue[];
  default?: JsonValue;
  items?: JsonSchema;
  properties?: Record<string, JsonSchema>;
  required?: string[];
  additionalProperties?: boolean | JsonSchema;
  [extension: string]: JsonValue | JsonSchema | JsonSchema[] | undefined;
};

export type DuckDbPrimitiveType =
  | 'null'
  | 'boolean'
  | 'tinyint'
  | 'smallint'
  | 'integer'
  | 'bigint'
  | 'hugeint'
  | 'utinyint'
  | 'usmallint'
  | 'uinteger'
  | 'ubigint'
  | 'float'
  | 'double'
  | 'decimal'
  | 'date'
  | 'time'
  | 'timestamp'
  | 'interval'
  | 'uuid'
  | 'varchar'
  | 'blob'
  | 'list'
  | 'struct'
  | 'map'
  | 'union'
  | 'unknown';

/** A column contract is the connection vocabulary for native extraction and plot nodes. */
export type TableColumnContract = {
  name: string;
  type?: string;
  primitive_type: DuckDbPrimitiveType;
  duckdb_type?: string;
  nullable?: boolean;
  description?: string;
  element?: TableColumnContract;
  fields?: TableColumnContract[];
};

export type TableContract = {
  table_contract_name: string;
  resource_id?: string;
  module_id?: string;
  domain?: string;
  description?: string;
  columns: TableColumnContract[];
};

export type CapabilityParameter = {
  name: string;
  label?: string;
  description?: string;
  required?: boolean;
  extensions?: string[];
  path_kind?: 'file' | 'directory' | 'file-or-directory';
  directory_extensions?: string[];
  schema: JsonSchema;
  default?: JsonValue;
  semantic_type?: string;
};

export type CapabilityInterface = {
  category?: string;
  invocation_model?: string;
  requires_connection?: boolean;
  guidance?: string;
  next_operations?: string[];
};

export type CapabilityEffects = {
  mutates_project?: boolean;
  reads?: string[] | TableContract[];
  writes?: string[] | TableContract[];
  conditional_reads?: string[] | TableContract[];
};

export type CapabilityPort = {
  id: string;
  label: string;
  direction: 'input' | 'output';
  required?: boolean;
  multiple?: boolean;
  accepts?: Array<'method' | 'result' | 'table' | 'scalar' | 'column' | 'plot' | 'project' | 'execution'>;
  table?: TableContract;
  schema?: JsonSchema;
  semantic_contract?: string;
  data_kind?: 'duckdb_table' | 'structured_value' | 'signal';
  representations?: string[];
  optional?: boolean;
};

export type OperationCanvasCapability = {
  node_kind: 'operation';
  outputs_to_canvas: true;
  icon?: string;
  input_ports?: CapabilityPort[];
  connection_guidance?: string;
  supports_preview?: boolean;
  supports_plot?: boolean;
};

/** Methods are workflow nodes only: they chain with methods and never emit canvas data. */
export type MethodWorkflowCapability = {
  node_kind: 'method';
  outputs_to_canvas: false;
  chainable: true;
  icon?: string;
  input_ports?: CapabilityPort[];
  connection_guidance?: string;
};

export type CanvasCapability = OperationCanvasCapability | MethodWorkflowCapability;

export type BackendCapabilityBase = {
  canonical_id: string;
  domain: string;
  label: string;
  definition: string;
  interface: CapabilityInterface;
  interface_guidance?: string;
  executable?: boolean;
  exposed?: boolean;
  parameters: CapabilityParameter[];
  result: { id?: string; schema?: JsonSchema };
  effects: CapabilityEffects;
  module_id?: string;
  canvas?: CanvasCapability;
  /** Native catalogue port names returned by the service capability endpoint. */
  inputs?: CapabilityPort[];
  outputs?: CapabilityPort[];
  input_ports?: CapabilityPort[];
  output_ports?: CapabilityPort[];
  project_entry?: boolean;
};

export type OperationCapability = BackendCapabilityBase & {
  kind: 'operation';
  mcp?: { name: string; input_schema: JsonSchema };
  method_schema?: never;
  single_occurrence?: never;
};

/** Control-plane API commands are not workflow/data-processing operations. */
export type ApiCommandCapability = BackendCapabilityBase & {
  kind: 'command';
  mcp?: { name: string; input_schema: JsonSchema };
  method_schema?: JsonSchema;
  single_occurrence?: never;
};

export type MethodCapability = BackendCapabilityBase & {
  kind: 'method';
  method_schema?: JsonSchema;
  single_occurrence?: boolean;
  mcp?: never;
};

export type BackendCapability = OperationCapability | ApiCommandCapability | MethodCapability;

export function isOperationCapability(capability: BackendCapability): capability is OperationCapability {
  return capability.kind === 'operation';
}

export function isMethodCapability(capability: BackendCapability): capability is MethodCapability {
  return capability.kind === 'method';
}

/** Frontend-owned nodes consume typed table columns without pretending to be backend operations. */
export type FrontendNodeCapability = {
  source: 'frontend';
  canonical_id: string;
  node_kind: 'extract' | 'plot';
  label: string;
  definition: string;
  accepted_primitives: DuckDbPrimitiveType[];
  input_ports: CapabilityPort[];
  output_ports: CapabilityPort[];
  domain?: string;
  plot?: { chart_type: string; input_result_types: string[]; required_columns?: string[] };
};

export type CapabilityIndexModule = {
  domain: string;
  module_id: string;
};

export type CapabilityIndexResponse = {
  protocol_version: string;
  domains: string[];
  modules: CapabilityIndexModule[];
};

export type CapabilityModulesResponse = {
  domain: string;
  modules: string[];
};

/** Lightweight operation metadata returned by filtered discovery. */
export type CapabilityOperationSummary = Pick<
  BackendCapabilityBase,
  'canonical_id' | 'label' | 'domain' | 'module_id' | 'definition'
>;

export type CapabilityOperationsRequest = {
  domain: string;
  module?: string;
  search?: string;
  includeSchema?: boolean;
};

export type ServiceCapabilities = {
  protocol_version: string;
  /** The current endpoint name is retained; entries are separated by `kind`. */
  operations: BackendCapability[];
  methods?: MethodCapability[];
  domains?: string[];
  tables?: TableContract[];
  frontend_nodes?: FrontendNodeCapability[];
  endpoints: string[];
};

export type DependencyDescriptor = {
  id: string;
  label: string;
  version: string;
  kind: string;
  required_by?: string[];
  managed_path: string;
  installable: boolean;
  network_required: boolean;
  available?: boolean;
};

export type DependencyInstallResult = {
  dependency_id: string;
  status: string;
  path?: string;
  message?: string;
};
