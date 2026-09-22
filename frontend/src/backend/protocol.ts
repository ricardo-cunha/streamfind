export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

export type StreamFindEvent = {
  type: string;
  project?: string | JsonValue;
  operation_id?: string;
  execution_id?: string;
  timestamp?: string;
  payload?: JsonValue;
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
  domain: string;
  metadata: Record<string, JsonValue>;
  initialization?: ProjectInitialization;
};

export type ProjectInitialization = {
  required: boolean;
  state: 'not_required' | 'awaiting_input' | 'running' | 'completed' | 'failed';
  operation_id?: string;
  operation?: OperationCapability;
};

export type WorkflowState =
  'idle' | 'validated' | 'queued' | 'running' | 'paused' | 'cancelling' | 'completed' | 'failed' | 'cancelled';

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
  domain: string;
  operations: WorkflowOperationDefinition[];
  connections: WorkflowConnectionDefinition[];
};

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
  primitive_type: DuckDbPrimitiveType;
  duckdb_type?: string;
  nullable?: boolean;
  description?: string;
  element?: TableColumnContract;
  fields?: TableColumnContract[];
};

export type TableContract = {
  table_name: string;
  module_id?: string;
  domain?: string;
  description?: string;
  columns: TableColumnContract[];
};

/** Ontology-defined result class exposed by a backend operation, e.g. `eicResult`. */
export type DomainResultContract = {
  canonical_id: string;
  label: string;
  definition: string;
  table: TableContract;
  schema?: JsonSchema;
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
  results?: DomainResultContract[];
  schema?: JsonSchema;
  semantic_contract?: string;
  data_kind?: 'duckdb_table' | 'tabular_value' | 'structured_value' | 'signal';
  representations?: string[];
  optional?: boolean;
};

export type OperationCanvasCapability = {
  node_kind: 'operation';
  outputs_to_canvas: true;
  output_results: DomainResultContract[];
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
  result: { schema?: JsonSchema; tables?: TableContract[]; domain_results?: DomainResultContract[] };
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
  cacheable?: never;
  single_occurrence?: never;
};

export type MethodCapability = BackendCapabilityBase & {
  kind: 'method';
  method_schema?: JsonSchema;
  cacheable?: boolean;
  single_occurrence?: boolean;
  mcp?: never;
};

export type BackendCapability = OperationCapability | MethodCapability;

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
