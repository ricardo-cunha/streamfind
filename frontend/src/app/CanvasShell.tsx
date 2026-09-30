import {
  Fragment,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  useCallback,
  type ChangeEvent,
  type MouseEvent as ReactMouseEvent,
} from 'react';
import {
  StreamFindApiClient,
  type ArtifactDataResponse,
  type ArtifactRecord,
  type ProjectSession,
} from '../backend/StreamFindApiClient';
import { PathFileManager } from './PathFileManager';
import { subscribeAppNotifications } from './notifications';
import { visualizationSpecFromArtifact } from '../visualization/VisualizationDataResolver';
import { VisualizationRenderer } from '../visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../visualization/visualizationTypes';
import type {
  BackendCapability,
  CapabilityParameter,
  CapabilityPort,
  JsonSchema,
  JsonValue,
  ServiceCapabilities,
  WorkflowDefinition,
  WorkflowConnectionDefinition,
  WorkflowOperationDefinition,
  WorkflowState,
} from '../backend/protocol';

type CanvasNodeKind = 'project' | 'operation' | 'method' | 'extract' | 'plot';
type CanvasNode = {
  id: string;
  kind: CanvasNodeKind;
  title: string;
  description: string;
  x: number;
  y: number;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
  parameters?: Record<string, unknown>;
  executionState?: 'idle' | 'running' | 'completed' | 'failed';
  executionMessage?: string;
};
type CanvasEdge = {
  id: string;
  source: string;
  sourcePort: string;
  target: string;
  targetPort: string;
};
type Point = { x: number; y: number };
type Interaction = {
  type: 'node' | 'pan';
  id?: string;
  offsetX?: number;
  offsetY?: number;
  originX?: number;
  originY?: number;
};
type ConnectionDraft = { source: string; sourcePort: string; point: Point };
type CanvasLogLine = { id: number; timestamp: string; message: string; level: 'info' | 'success' | 'error' };
type CanvasHistorySnapshot = { nodes: CanvasNode[]; edges: CanvasEdge[] };

function workflowLayout(index: number): Point {
  return { x: 700 + (index % 3) * 420, y: 650 + Math.floor(index / 3) * 340 };
}

function canonicalWorkflowValue(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalWorkflowValue);
  if (value && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, item]) => [key, canonicalWorkflowValue(item)]),
    );
  }
  return value;
}

function workflowFingerprint(workflow: WorkflowDefinition): string {
  return JSON.stringify(
    canonicalWorkflowValue({
      schema_version: workflow.schema_version,
      workflow_id: workflow.workflow_id || 'workflow',
      operations: workflow.operations,
      connections: workflow.connections,
    }),
  );
}

function canvasWorkflow(
  nodes: CanvasNode[],
  edges: CanvasEdge[],
  capabilities: ServiceCapabilities,
  revision: number,
): WorkflowDefinition {
  const operations: WorkflowOperationDefinition[] = nodes
    .filter((node) => node.capabilityId)
    .map((node) => ({
      id: node.id,
      operation: node.capabilityId as string,
      parameters: wireParameters(
        capabilities.operations.find((capability) => capability.canonical_id === node.capabilityId),
        node.parameters || {},
      ),
      position: { x: node.x, y: node.y },
    }));
  const connections: WorkflowConnectionDefinition[] = edges.map((edge) => ({
    source_operation: edge.source,
    source_port: edge.sourcePort,
    target_operation: edge.target,
    target_port: edge.targetPort,
  }));
  return {
    schema_version: 1,
    workflow_id: 'workflow',
    name: 'Workflow',
    version: revision,
    operations,
    connections,
  };
}

type NodeTemplate = {
  id: string;
  kind: Exclude<CanvasNodeKind, 'project'>;
  title: string;
  description: string;
  icon: string;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
  domain?: string;
  module?: string;
};

type DocumentationFocus = { section: 'inputs' | 'outputs' | 'parameters'; key: string };

function operationDeckDensity(title: string, metadata: string): 'regular' | 'compact' | 'dense' {
  const totalLength = title.length + metadata.length;
  if (title.length > 36 || totalLength > 78) return 'dense';
  if (title.length > 25 || totalLength > 58) return 'compact';
  return 'regular';
}

function capabilityTemplates(capabilities: ServiceCapabilities): NodeTemplate[] {
  const backend = capabilities.operations
    .filter((capability) => capability.kind === 'operation')
    .map((capability: BackendCapability) => ({
      id: capability.canonical_id,
      kind: 'operation' as const,
      title: capability.label,
      description: capability.definition,
      icon: capability.kind === 'operation' ? 'fa-solid fa-bolt' : 'fa-solid fa-gears',
      outputsToCanvas: capability.kind === 'operation',
      capabilityId: capability.canonical_id,
      projectEntry: capability.project_entry === true,
      domain: capability.domain,
      module: capability.module_id,
    }));
  return backend;
}

function defaultParameters(capability: BackendCapability): Record<string, unknown> {
  return Object.fromEntries(
    capability.parameters.map((parameter) => [
      parameter.name,
      parameter.default ??
        parameter.schema.default ??
        (parameter.schema.type === 'array' || parameter.schema.type === 'table' ? [] : ''),
    ]),
  );
}

function schemaTypeLabel(schema: JsonSchema | undefined): string {
  if (!schema) return 'unknown';
  const type = Array.isArray(schema.type) ? schema.type.join(' | ') : schema.type || 'value';
  if (type === 'array' && schema.items) return `array<${schemaTypeLabel(schema.items)}>`;
  return type;
}

function schemaOntologyDetails(schema: JsonSchema | undefined): string[] {
  if (!schema) return [];
  const details: string[] = [];
  const pathKind = schema.path_kind;
  const extensions = schema.extensions;
  if (typeof pathKind === 'string') details.push(`path kind: ${pathKind}`);
  if (Array.isArray(extensions) && extensions.every((extension) => typeof extension === 'string'))
    details.push(`extensions: ${extensions.join(', ')}`);
  return details;
}

function portOntologyDetails(port: NodePort): string[] {
  return [`type: ${port.schema ? schemaTypeLabel(port.schema) : port.dataKind || 'value'}`];
}

function parameterOntologyDetails(parameter: CapabilityParameter): string[] {
  const details = schemaOntologyDetails(parameter.schema);
  const itemSchema = parameter.schema.items;
  const itemExtensions =
    itemSchema && Array.isArray(itemSchema.extensions)
      ? itemSchema.extensions.filter((extension): extension is string => typeof extension === 'string')
      : [];
  const itemPathKind = itemSchema && typeof itemSchema.path_kind === 'string' ? itemSchema.path_kind : undefined;
  if (itemPathKind) details.push(`path kind: ${itemPathKind}`);
  if (itemExtensions.length) details.push(`allowed extensions: ${itemExtensions.join(', ')}`);
  if (parameter.path_kind) details.push(`path kind: ${parameter.path_kind}`);
  if (parameter.extensions?.length) details.push(`allowed extensions: ${parameter.extensions.join(', ')}`);
  if (parameter.directory_extensions?.length)
    details.push(`directory extensions: ${parameter.directory_extensions.join(', ')}`);
  return Array.from(new Set(details));
}

function ontologyPortTerm(port: NodePort): string {
  return port.table?.table_name || port.semanticContract || port.id;
}

function tableRowsToWireValue(value: unknown): unknown {
  // Row-oriented table values are the public operation form. Preserve their
  // scalar JSON types; converting every cell to a string loses numeric target
  // values such as mass and m/z. The native validator also accepts the
  // persisted column-oriented form when one is supplied directly.
  return value;
}

function wireParameters(
  capability: BackendCapability | undefined,
  parameters: Record<string, unknown>,
): Record<string, JsonValue> {
  if (!capability) return JSON.parse(JSON.stringify(parameters)) as Record<string, JsonValue>;
  return Object.fromEntries(
    Object.entries(parameters)
      .filter(([name]) => capability.parameters.some((candidate) => candidate.name === name))
      .map(([name, value]) => {
        const parameter = capability.parameters.find((candidate) => candidate.name === name);
        return [name, parameter?.schema.type === 'table' ? tableRowsToWireValue(value) : value];
      }),
  ) as Record<string, JsonValue>;
}

function wireTableToRows(value: unknown): unknown {
  if (
    !value ||
    typeof value !== 'object' ||
    Array.isArray(value) ||
    !Array.isArray((value as { columns?: unknown }).columns)
  )
    return value;
  const columns = (value as { columns: Array<{ name: string; values?: unknown[] }> }).columns;
  const rowCount = Math.max(0, ...columns.map((column) => column.values?.length || 0));
  return Array.from({ length: rowCount }, (_, index) =>
    Object.fromEntries(columns.map((column) => [column.name, column.values?.[index] ?? null])),
  );
}

function uiParameters(
  capability: BackendCapability | undefined,
  parameters: Record<string, unknown>,
): Record<string, unknown> {
  if (!capability) return parameters;
  return Object.fromEntries(
    Object.entries(parameters).map(([name, value]) => {
      const parameter = capability.parameters.find((candidate) => candidate.name === name);
      return [name, parameter?.schema.type === 'table' ? wireTableToRows(value) : value];
    }),
  );
}

function fileParameterExtensions(parameter: CapabilityParameter): string[] {
  if (parameter.extensions?.length) return parameter.extensions;
  const itemExtensions = parameter.schema.items?.extensions;
  return Array.isArray(itemExtensions) ? itemExtensions.filter((item): item is string => typeof item === 'string') : [];
}

function isSimplePathParameter(parameter: CapabilityParameter): boolean {
  return (
    parameter.schema.type === 'array' &&
    parameter.schema.items?.type !== 'object' &&
    Boolean(
      parameter.schema.items?.path_kind ||
      parameter.schema.items?.extensions ||
      parameter.schema.items?.type === 'path',
    )
  );
}

function isFileListParameter(parameter: CapabilityParameter): boolean {
  if (parameter.schema.type !== 'array') return false;
  if (parameter.schema.items?.type !== 'object') return false;
  if (parameter.extensions?.length || parameter.path_kind) return true;
  const properties = parameter.schema.items.properties;
  return Boolean(
    properties &&
    Object.values(properties).some(
      (property) => property.type === 'path' || property.format === 'path' || property.format === 'file-path',
    ),
  );
}

function isPathListParameter(parameter: CapabilityParameter): boolean {
  return isSimplePathParameter(parameter) || isFileListParameter(parameter);
}

function isTableParameter(parameter: CapabilityParameter): boolean {
  return parameter.schema.type === 'table' && Boolean(parameter.schema.properties);
}

function schemaPropertyOrder(schema: JsonSchema): string[] {
  const properties = schema.properties || {};
  const declared = schema['x-streamfind-property-order'];
  const ordered = Array.isArray(declared) ? declared.filter((item): item is string => typeof item === 'string') : [];
  return [
    ...ordered.filter((name) => name in properties),
    ...Object.keys(properties).filter((name) => !ordered.includes(name)),
  ];
}

function validateInlineTableRows(parameter: CapabilityParameter, rows: Record<string, unknown>[]): string | null {
  const properties = parameter.schema.properties || {};
  const allowed = new Set(Object.keys(properties));
  const required = new Set(parameter.schema.required || []);
  for (let rowIndex = 0; rowIndex < rows.length; rowIndex += 1) {
    const row = rows[rowIndex];
    const unknown = Object.keys(row).find((name) => !allowed.has(name));
    if (unknown) return `Row ${rowIndex + 1}: unknown column “${unknown}”.`;
    for (const name of required) {
      if (row[name] === undefined || row[name] === null || row[name] === '')
        return `Row ${rowIndex + 1}: required column “${name}” is empty.`;
    }
    for (const [name, value] of Object.entries(row)) {
      if (value === '' || value === null || value === undefined) continue;
      const declared = properties[name]?.type;
      const types = Array.isArray(declared) ? declared : [declared];
      if (types.includes('number') && Number.isNaN(Number(value)))
        return `Row ${rowIndex + 1}: “${name}” must be a number.`;
      if (types.includes('integer') && (!Number.isInteger(Number(value)) || Number.isNaN(Number(value))))
        return `Row ${rowIndex + 1}: “${name}” must be an integer.`;
      if (types.includes('boolean') && value !== true && value !== false && value !== 'true' && value !== 'false')
        return `Row ${rowIndex + 1}: “${name}” must be boolean.`;
    }
  }
  return null;
}

function normalizeInlineTableRows(parameter: CapabilityParameter, rows: Record<string, unknown>[]) {
  const properties = parameter.schema.properties || {};
  return rows.map((row) =>
    Object.fromEntries(
      Object.entries(row).map(([name, value]) => {
        const type = properties[name]?.type;
        if (type === 'number' || type === 'integer') return [name, Number(value)];
        if (type === 'boolean' && (value === 'true' || value === 'false')) return [name, value === 'true'];
        return [name, value];
      }),
    ),
  );
}

function parseCsvRows(text: string, columns: string[]): { rows?: Record<string, unknown>[]; error?: string } {
  const records = text
    .split(/\r?\n/)
    .filter((line) => line.trim().length > 0)
    .map((line) => {
      const cells: string[] = [];
      let cell = '';
      let quoted = false;
      for (let index = 0; index < line.length; index += 1) {
        const character = line[index];
        if (character === '"' && line[index + 1] === '"') {
          cell += '"';
          index += 1;
        } else if (character === '"') quoted = !quoted;
        else if (character === ',' && !quoted) {
          cells.push(cell.trim());
          cell = '';
        } else cell += character;
      }
      cells.push(cell.trim());
      return cells;
    });
  if (!records.length) return { error: 'CSV is empty.' };
  const headers = records[0];
  const unknown = headers.find((header) => !columns.includes(header));
  if (unknown) return { error: `CSV contains unknown column “${unknown}”.` };
  const missing = columns.find((column) => !headers.includes(column));
  if (missing) return { error: `CSV is missing declared column “${missing}”.` };
  return {
    rows: records
      .slice(1)
      .map((cells) => Object.fromEntries(columns.map((column) => [column, cells[headers.indexOf(column)] ?? '']))),
  };
}

function isJsonParameter(parameter: CapabilityParameter): boolean {
  return (
    parameter.schema.type === 'table' ||
    parameter.schema.type === 'object' ||
    (parameter.schema.type === 'array' && !isFileListParameter(parameter))
  );
}

function validateJsonValue(value: unknown, schema: JsonSchema, path = 'value'): string | null {
  const declared = Array.isArray(schema.type) ? schema.type : [schema.type];
  if (!declared.length || declared.includes(undefined)) return null;
  const matches = (type: string): string | null => {
    if (type === 'table') {
      if (!Array.isArray(value)) return `${path} must be an array of table rows.`;
      for (let index = 0; index < value.length; index += 1) {
        const row = value[index];
        if (!row || typeof row !== 'object' || Array.isArray(row)) return `${path}[${index}] must be an object.`;
        const properties = schema.properties || {};
        for (const required of schema.required || []) {
          if (!(required in row)) return `${path}[${index}] is missing required column “${required}”.`;
        }
        for (const [name, item] of Object.entries(row)) {
          if (properties[name]) {
            const error = validateJsonValue(item, properties[name] as JsonSchema, `${path}[${index}].${name}`);
            if (error) return error;
          } else if (schema.additionalProperties === false) {
            return `${path}[${index}] contains unknown column “${name}”.`;
          }
        }
      }
      return null;
    }
    if (type === 'array') {
      if (!Array.isArray(value)) return `${path} must be an array.`;
      if (!schema.items) return null;
      for (let index = 0; index < value.length; index += 1) {
        const error = validateJsonValue(value[index], schema.items, `${path}[${index}]`);
        if (error) return error;
      }
      return null;
    }
    if (type === 'object') {
      if (!value || typeof value !== 'object' || Array.isArray(value)) return `${path} must be an object.`;
      const object = value as Record<string, unknown>;
      for (const required of schema.required || []) {
        if (!(required in object)) return `${path} is missing required property “${required}”.`;
      }
      for (const [name, item] of Object.entries(object)) {
        if (schema.properties?.[name]) {
          const error = validateJsonValue(item, schema.properties[name] as JsonSchema, `${path}.${name}`);
          if (error) return error;
        } else if (schema.additionalProperties === false) {
          return `${path} contains unknown property “${name}”.`;
        }
      }
      return null;
    }
    if (type === 'string' || type === 'path') return typeof value === 'string' ? null : `${path} must be a string.`;
    if (type === 'boolean') return typeof value === 'boolean' ? null : `${path} must be boolean.`;
    if (type === 'integer') return Number.isInteger(value) ? null : `${path} must be an integer.`;
    if (type === 'number' || type === 'real' || type === 'float' || type === 'double')
      return typeof value === 'number' && Number.isFinite(value) ? null : `${path} must be a finite number.`;
    if (type === 'null') return value === null ? null : `${path} must be null.`;
    return null;
  };
  for (const type of declared) {
    const error = matches(type || 'value');
    if (!error) return null;
  }
  return `Invalid ${schemaTypeLabel(schema)} at ${path}.`;
}

function scalarInputValue(parameter: CapabilityParameter, raw: string): unknown {
  const type = Array.isArray(parameter.schema.type) ? parameter.schema.type[0] : parameter.schema.type;
  if (type === 'integer') return raw === '' ? '' : Number.parseInt(raw, 10);
  if (type === 'number' || type === 'real' || type === 'float' || type === 'double')
    return raw === '' ? '' : Number.parseFloat(raw);
  if (type === 'boolean') return raw === 'true';
  return raw;
}

type NodePort = {
  id: string;
  label: string;
  required?: boolean;
  description?: string;
  table?: CapabilityPort['table'];

  semanticContract?: string;
  dataKind?: CapabilityPort['data_kind'];
  schema?: JsonSchema;
  representations?: string[];
  typeKey: string;
};

function portTypeKey(port: CapabilityPort | NodePort): string {
  const dataKind = 'dataKind' in port ? port.dataKind : (port as CapabilityPort).data_kind;
  const contract = 'semanticContract' in port ? port.semanticContract : (port as CapabilityPort).semantic_contract;
  if (dataKind && contract) return `${dataKind}:${contract}`;
  if (dataKind) return dataKind;
  if (contract) return contract;
  const representations = 'representations' in port ? port.representations : undefined;
  if (representations?.length) return representations.slice().sort().join('|');
  if (port.schema?.type) {
    const schemaType = Array.isArray(port.schema.type) ? port.schema.type.join('|') : port.schema.type;
    if (schemaType === 'array' || schemaType === 'object') return 'structured_value';
    if (schemaType === 'table') return 'table';
    return schemaType;
  }
  return 'unknown';
}

function visualPortTypeKey(port: NodePort): string {
  const schemaType = Array.isArray(port.schema?.type) ? port.schema.type[0] : port.schema?.type;
  if (schemaType === 'array') return schemaTypeLabel(port.schema);
  if (schemaType === 'object') return 'object';
  if (schemaType === 'table') return 'table';
  return port.typeKey;
}

function parameterTypeKey(parameter: CapabilityParameter): string {
  const type = Array.isArray(parameter.schema.type) ? parameter.schema.type.join('|') : parameter.schema.type;
  if (type === 'table') return 'table';
  if (type === 'array') return schemaTypeLabel(parameter.schema);
  if (type === 'object') return 'object';
  return type || 'unknown';
}

function schemaShape(schema: JsonSchema | undefined): string {
  if (!schema) return '';
  const normalized: Record<string, unknown> = {};
  for (const key of ['type', 'properties', 'items', 'required', 'additionalProperties', 'enum']) {
    if (schema[key] !== undefined) normalized[key] = schema[key];
  }
  if (normalized.properties && typeof normalized.properties === 'object') {
    normalized.properties = Object.fromEntries(
      Object.entries(normalized.properties as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, value]) => {
          const property = value as JsonSchema;
          const columnSchema =
            normalized.type === 'table' && property.type === 'array' && property.items ? property.items : property;
          return [key, schemaShape(columnSchema)];
        }),
    );
  }
  if (normalized.items && typeof normalized.items === 'object') {
    normalized.items = schemaShape(normalized.items as JsonSchema);
  }
  return JSON.stringify(normalized);
}

function typeIcon(typeKey: string): string {
  const type = typeKey.toLowerCase();
  if (type.includes('duckdb_table') || type === 'table') return 'fa-solid fa-table';
  if (type.startsWith('array<')) {
    if (type.includes('integer') || type.includes('number') || type.includes('real') || type.includes('double'))
      return 'fa-solid fa-list-ol';
    if (type.includes('boolean')) return 'fa-solid fa-list-check';
    if (type.includes('string') || type.includes('varchar')) return 'fa-solid fa-list';
    return 'fa-solid fa-layer-group';
  }
  if (type.includes('json') || type.includes('object') || type.includes('array') || type.includes('structured_value'))
    return 'fa-solid fa-code';
  if (type.includes('table') || type.includes('result')) return 'fa-solid fa-table';
  if (type.includes('path') || type.includes('file')) return 'fa-solid fa-file';
  if (type.includes('bool')) return 'fa-solid fa-toggle-on';
  if (type.includes('int') || type.includes('real') || type.includes('float') || type.includes('double'))
    return 'fa-solid fa-hashtag';
  if (type.includes('string') || type.includes('text') || type.includes('varchar')) return 'fa-solid fa-font';
  return 'fa-solid fa-circle-nodes';
}

function typeClass(typeKey: string): string {
  const type = typeKey.toLowerCase();
  if (type.includes('duckdb_table') || type === 'table') return 'sf-port-type-table';
  if (type.startsWith('array<')) {
    if (type.includes('integer') || type.includes('number') || type.includes('real') || type.includes('double'))
      return 'sf-port-type-array-number';
    if (type.includes('boolean')) return 'sf-port-type-array-boolean';
    if (type.includes('string') || type.includes('varchar')) return 'sf-port-type-array-string';
    return 'sf-port-type-array-structured';
  }
  if (type.includes('json') || type.includes('object') || type.includes('array') || type.includes('structured_value'))
    return 'sf-port-type-json';
  if (type.includes('table') || type.includes('result')) return 'sf-port-type-table';
  if (type.includes('path') || type.includes('file')) return 'sf-port-type-path';
  if (type.includes('bool')) return 'sf-port-type-boolean';
  if (type.includes('int') || type.includes('real') || type.includes('float') || type.includes('double'))
    return 'sf-port-type-number';
  if (type.includes('string') || type.includes('text') || type.includes('varchar')) return 'sf-port-type-string';
  return 'sf-port-type-unknown';
}

function artifactSummary(artifact: ArtifactRecord): string {
  if (artifact.representation === 'table') {
    const columns = artifact.columns || [];
    const names = columns
      .slice(0, 6)
      .map((column) => column.name)
      .join(', ');
    return `${artifact.physical_table || 'DuckDB table'}\n${columns.length} columns · ${artifact.row_count ?? 0} rows\n${names}${columns.length > 6 ? ', …' : ''}`;
  }
  let payload = artifact.payload;
  if (typeof payload === 'string') {
    try {
      payload = JSON.parse(payload) as JsonValue;
    } catch {
      // Keep non-JSON scalar strings unchanged.
    }
  }
  const bounded = Array.isArray(payload)
    ? payload.length > 8
      ? [...payload.slice(0, 8), `… ${payload.length - 8} more items`]
      : payload
    : payload && typeof payload === 'object'
      ? Object.fromEntries(Object.entries(payload).slice(0, 8))
      : payload;
  const rendered = JSON.stringify(bounded, null, 2) ?? String(bounded);
  const preview = rendered.length > 420 ? `${rendered.slice(0, 417)}…` : rendered;
  return `JSON\n${preview}`;
}

function prettyArtifactPayload(payload: ArtifactRecord['payload']): string {
  if (typeof payload !== 'string') return JSON.stringify(payload ?? null, null, 2);
  try {
    return JSON.stringify(JSON.parse(payload) as JsonValue, null, 2);
  } catch {
    return payload;
  }
}

function VisualizationArtifactPreview({ artifact }: { artifact: ArtifactRecord }) {
  let spec: VisualizationSpec;
  try {
    spec = visualizationSpecFromArtifact(artifact);
  } catch {
    return <pre className="sf-artifact-viewer-json">{prettyArtifactPayload(artifact.payload)}</pre>;
  }
  return <VisualizationRenderer spec={spec} className="sf-visualization-preview" />;
}

function artifactContractMatches(artifactContract: string, portContract?: string): boolean {
  if (!portContract) return false;
  if (artifactContract === portContract) return true;
  const local = (value: string) => value.split('#').at(-1)?.split(':').at(-1) ?? value;
  return local(artifactContract) === local(portContract);
}

function isVisualizationArtifact(artifact: ArtifactRecord): boolean {
  const contract = artifact.contract_id.split('#').at(-1)?.split(':').at(-1) ?? artifact.contract_id;
  return contract === 'visualizationSpecResult';
}

function nodePorts(capability: BackendCapability | undefined): { inputs: NodePort[]; outputs: NodePort[] } {
  const canvas = capability?.canvas;
  const inputPorts = canvas?.input_ports || capability?.input_ports || capability?.inputs || [];
  const outputPorts = capability?.output_ports || capability?.outputs || [];

  const inputs = inputPorts.map((port: CapabilityPort) => ({
    id: port.id,
    label: port.label || port.id,
    required: port.required ?? port.optional !== true,
    description: port.schema?.description,
    table: port.table,
    semanticContract: port.semantic_contract,
    dataKind: port.data_kind,
    schema: port.schema,
    representations: port.representations,
    typeKey: portTypeKey(port),
  }));
  const outputs = [
    ...outputPorts.map((port: CapabilityPort) => ({
      id: port.id,
      label: port.label || port.id,
      description: port.schema?.description,
      table: port.table,
      semanticContract: port.semantic_contract,
      dataKind: port.data_kind,
      schema: port.schema,
      representations: port.representations,
      typeKey: portTypeKey(port),
    })),
  ];
  return { inputs, outputs };
}

function portMatchesParameter(sourcePort: NodePort, parameter: CapabilityParameter): boolean {
  const parameterKind =
    parameter.schema.type === 'table'
      ? 'table'
      : parameter.schema.type === 'object' || parameter.schema.type === 'array'
        ? 'structured_value'
        : parameter.schema.type;
  return schemaShape(sourcePort.schema) === schemaShape(parameter.schema) && sourcePort.dataKind === parameterKind;
}

const NODE_WIDTH = 224;
const NODE_HEIGHT = 260;
const CONNECTED_NODE_GAP = 72;
const CANVAS_WORLD_WIDTH = 6000;
const CANVAS_WORLD_HEIGHT = 4000;
const GRID_SIZE = 22;

function connectedNodePosition(source: CanvasNode, nodes: CanvasNode[]): Point {
  const columnStep = NODE_WIDTH + CONNECTED_NODE_GAP;
  const rowStep = NODE_HEIGHT + CONNECTED_NODE_GAP;
  for (let column = 1; column <= 20; column += 1) {
    const rowOffsets = [0];
    for (let row = 1; row < 20; row += 1) rowOffsets.push(row, -row);
    for (const row of rowOffsets) {
      const candidate = {
        x: source.x + column * columnStep,
        y: source.y + row * rowStep,
      };
      const overlaps = nodes.some(
        (node) =>
          candidate.x < node.x + NODE_WIDTH &&
          candidate.x + NODE_WIDTH > node.x &&
          candidate.y < node.y + NODE_HEIGHT &&
          candidate.y + NODE_HEIGHT > node.y,
      );
      if (!overlaps) return candidate;
    }
  }
  return { x: source.x + columnStep, y: source.y };
}

const ARTIFACT_PAGE_SIZE = 1000;

function VirtualArtifactTable({
  columns,
  pages,
  pageOffset,
  loading,
}: {
  columns: ArtifactDataResponse['columns'];
  pages: Record<number, ArtifactDataResponse>;
  pageOffset: number;
  loading: boolean;
}) {
  const rows = pages[pageOffset]?.rows ?? [];

  const columnWidth = 160;
  return (
    <div className="sf-artifact-virtual-scroll" role="region" aria-label="Artifact table" tabIndex={0}>
      <table>
        <colgroup>
          {columns.map((column) => (
            <col key={column.name} style={{ width: columnWidth }} />
          ))}
        </colgroup>
        <thead>
          <tr>
            {columns.map((column) => (
              <th key={column.name}>
                {column.name}
                <small>{column.type}</small>
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row, index) => {
            return (
              <tr key={index}>
                {columns.map((column) => (
                  <td key={column.name}>{row[column.name] ?? 'NULL'}</td>
                ))}
              </tr>
            );
          })}
        </tbody>
      </table>
      {loading ? <div className="sf-artifact-viewer-loading">Refreshing table…</div> : null}
    </div>
  );
}

export default function CanvasShell({
  project,
  capabilities,
  surface,
  client,
  onProjectHub,
  onOpenOntologyWiki,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  surface: 'workflow' | 'explorer';
  client?: StreamFindApiClient;
  onProjectHub?: () => void;
  onOpenOntologyWiki?: (term?: string) => void;
}) {
  const canvasRef = useRef<HTMLDivElement | null>(null);
  const workflowFileInputRef = useRef<HTMLInputElement | null>(null);
  const nextId = useRef(1);
  const [nodes, setNodes] = useState<CanvasNode[]>([]);
  const [edges, setEdges] = useState<CanvasEdge[]>([]);
  const [scale, setScale] = useState(1);
  const [offset, setOffset] = useState<Point>({ x: 0, y: 0 });
  const initialViewportFittedRef = useRef(false);
  const [worldSize, setWorldSize] = useState({ width: CANVAS_WORLD_WIDTH, height: CANVAS_WORLD_HEIGHT });
  const viewportRef = useRef({ offset, scale });
  useEffect(() => {
    viewportRef.current = { offset, scale };
  }, [offset, scale]);
  const canvasWorldSize = useMemo(() => {
    const requiredWidth = Math.max(CANVAS_WORLD_WIDTH, ...nodes.map((node) => node.x + NODE_WIDTH + 800));
    const requiredHeight = Math.max(CANVAS_WORLD_HEIGHT, ...nodes.map((node) => node.y + NODE_HEIGHT + 600));
    return {
      width: Math.max(worldSize.width, requiredWidth),
      height: Math.max(worldSize.height, requiredHeight),
    };
  }, [nodes, worldSize]);
  const [interaction, setInteraction] = useState<Interaction | null>(null);
  const [connection, setConnection] = useState<ConnectionDraft | null>(null);
  const pendingConnectionRef = useRef<{
    source: string;
    sourcePort: string;
    startX: number;
    startY: number;
  } | null>(null);
  const [picker, setPicker] = useState<(Point & { source?: string; sourcePort?: string }) | null>(null);
  const [pickerCapabilityId, setPickerCapabilityId] = useState<string | null>(null);
  const [jsonEditor, setJsonEditor] = useState<{ nodeId: string; parameter: CapabilityParameter } | null>(null);
  const [pathWizard, setPathWizard] = useState<{
    nodeId: string;
    parameter: CapabilityParameter;
    mergeIntoJsonEditor?: boolean;
  } | null>(null);

  const [tableEditor, setTableEditor] = useState<{
    nodeId: string;
    parameter: CapabilityParameter;
    rows: Record<string, unknown>[];
    returnToJsonEditor?: boolean;
    error?: string | null;
  } | null>(null);
  const [csvPreview, setCsvPreview] = useState<{ rows: Record<string, unknown>[]; error?: string } | null>(null);
  const [jsonEditorText, setJsonEditorText] = useState('');
  const [jsonEditorError, setJsonEditorError] = useState<string | null>(null);
  const [gridVisible, setGridVisible] = useState(false);
  const [workflowState, setWorkflowState] = useState<WorkflowState>('idle');
  const [, setWorkflowProgress] = useState({ completed: 0, total: 0, current_step: 0 });
  const [workflowBusy, setWorkflowBusy] = useState(false);
  const [workflowRevision, setWorkflowRevision] = useState(1);
  const [workflowLoaded, setWorkflowLoaded] = useState(surface !== 'workflow' || !client);
  const [savedWorkflowFingerprint, setSavedWorkflowFingerprint] = useState<string | null>(null);

  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [documentationFocus, setDocumentationFocus] = useState<DocumentationFocus | null>(null);
  const [selectedEdgeId, setSelectedEdgeId] = useState<string | null>(null);
  const lastStatusRef = useRef<{ message: string; timestamp: number } | null>(null);
  const recentEventRef = useRef<Map<string, number>>(new Map());
  const [paletteSearch, setPaletteSearch] = useState('');
  const [paletteModule, setPaletteModule] = useState('');
  const [anchorCenters, setAnchorCenters] = useState<Record<string, Point>>({});
  const [expandedParameters, setExpandedParameters] = useState<Record<string, boolean>>({});
  const [status, setStatusState] = useState(
    surface === 'workflow' ? 'Drag an output connector to a compatible input connector.' : 'Workflow canvas',
  );
  const [activityLog, setActivityLog] = useState<CanvasLogLine[]>([]);
  const [currentArtifacts, setCurrentArtifacts] = useState<ArtifactRecord[]>([]);
  const [artifactViewer, setArtifactViewer] = useState<ArtifactRecord | null>(null);
  const [artifactData, setArtifactData] = useState<ArtifactDataResponse | null>(null);
  const [artifactPages, setArtifactPages] = useState<Record<number, ArtifactDataResponse>>({});
  const [artifactViewerSearch, setArtifactViewerSearch] = useState('');
  const [artifactViewerSort, setArtifactViewerSort] = useState('');
  const [artifactViewerDescending, setArtifactViewerDescending] = useState(false);
  const [artifactViewerLoading, setArtifactViewerLoading] = useState(false);
  const historyRef = useRef<{ past: CanvasHistorySnapshot[]; future: CanvasHistorySnapshot[]; current: string }>({
    past: [],
    future: [],
    current: '',
  });
  const historyApplyingRef = useRef(false);
  const appendLog = useCallback((message: string, level: CanvasLogLine['level'] = 'info') => {
    setActivityLog((current) => [
      { id: Date.now() + current.length, timestamp: new Date().toLocaleTimeString(), message, level },
      ...current.slice(0, 99),
    ]);
  }, []);
  useEffect(() => {
    return subscribeAppNotifications((notification) => {
      const level: CanvasLogLine['level'] =
        notification.kind === 'error' ? 'error' : notification.kind === 'success' ? 'success' : 'info';
      appendLog(`notification: ${notification.message}`, level);
    });
  }, [appendLog]);
  const setStatus = useCallback(
    (message: string) => {
      const previous = lastStatusRef.current;
      const now = Date.now();
      if (previous?.message === message && now - previous.timestamp < 1000) return;
      lastStatusRef.current = { message, timestamp: now };
      setStatusState(message);
      appendLog(
        message,
        message.toLowerCase().includes('failed') || message.toLowerCase().includes('error') ? 'error' : 'info',
      );
    },
    [appendLog],
  );
  const refreshArtifacts = useCallback(() => {
    if (!client || typeof client.currentArtifacts !== 'function') return Promise.resolve<ArtifactRecord[]>([]);
    return client
      .currentArtifacts(project.session_id)
      .then((artifacts) => {
        setCurrentArtifacts(artifacts);
        return artifacts;
      })
      .catch(() => []);
  }, [client, project.session_id]);
  const artifactPageRequests = useRef(new Set<number>());
  const artifactPageQueryRef = useRef('');
  const requestArtifactPage = useCallback(
    (offset: number) => {
      if (!artifactViewer || artifactViewer.representation !== 'table' || !client) return;
      const queryKey = `${artifactViewer.artifact_id}|${artifactViewerSearch}|${artifactViewerSort}|${artifactViewerDescending}`;
      if (offset === 0 && artifactPageQueryRef.current !== queryKey) {
        artifactPageQueryRef.current = queryKey;
        artifactPageRequests.current.clear();
      }
      if (artifactPageRequests.current.has(offset)) return;
      artifactPageRequests.current.add(offset);
      setArtifactViewerLoading(true);
      void client
        .artifactData(project.session_id, {
          artifact_id: artifactViewer.artifact_id,
          limit: ARTIFACT_PAGE_SIZE,
          offset,
          search: artifactViewerSearch,
          sort_column: artifactViewerSort,
          descending: artifactViewerDescending,
        })
        .then((result) => {
          setArtifactData(result);
          setArtifactPages((current) => {
            const next = { ...current, [offset]: result };
            return Object.fromEntries(
              Object.entries(next).filter(
                ([pageOffset]) => Math.abs(Number(pageOffset) - offset) <= ARTIFACT_PAGE_SIZE * 3,
              ),
            );
          });
        })
        .catch(() => undefined)
        .finally(() => {
          artifactPageRequests.current.delete(offset);
          setArtifactViewerLoading(false);
        });
    },
    [artifactViewer, artifactViewerDescending, artifactViewerSearch, artifactViewerSort, client, project.session_id],
  );
  useEffect(() => {
    if (!artifactViewer || artifactViewer.representation !== 'table' || !client) return;
    const timer = window.setTimeout(() => requestArtifactPage(0), 300);
    return () => window.clearTimeout(timer);
  }, [artifactViewer, client, requestArtifactPage]);
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      if (artifactViewer) {
        setArtifactViewer(null);
        return;
      }
      if (jsonEditor || pathWizard || tableEditor) {
        setJsonEditor(null);
        setPathWizard(null);
        setTableEditor(null);
        return;
      }
      if (selectedNodeId || pickerCapabilityId) {
        setSelectedNodeId(null);
        setPickerCapabilityId(null);
        return;
      }
      if (picker) {
        setPicker(null);
        return;
      }
      setConnection(null);
      pendingConnectionRef.current = null;
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => window.removeEventListener('keydown', closeOnEscape);
  }, [artifactViewer, jsonEditor, pathWizard, picker, pickerCapabilityId, selectedNodeId, tableEditor]);
  const nodeTemplates = useMemo(() => capabilityTemplates(capabilities), [capabilities]);
  const currentWorkflow = useMemo(
    () => canvasWorkflow(nodes, edges, capabilities, workflowRevision),
    [capabilities, edges, nodes, workflowRevision],
  );
  const currentWorkflowFingerprint = useMemo(() => workflowFingerprint(currentWorkflow), [currentWorkflow]);

  useEffect(() => {
    if (!workflowLoaded) return;
    const snapshot = { nodes, edges };
    const fingerprint = JSON.stringify(snapshot);
    if (historyApplyingRef.current) {
      historyApplyingRef.current = false;
      historyRef.current.current = fingerprint;
      return;
    }
    if (!historyRef.current.current) {
      historyRef.current.current = fingerprint;
      return;
    }
    if (historyRef.current.current === fingerprint) return;
    historyRef.current.past = [
      ...historyRef.current.past.slice(-49),
      JSON.parse(historyRef.current.current) as CanvasHistorySnapshot,
    ];
    historyRef.current.future = [];
    historyRef.current.current = fingerprint;
  }, [edges, nodes, workflowLoaded]);

  const restoreHistorySnapshot = (snapshot: CanvasHistorySnapshot) => {
    historyApplyingRef.current = true;
    setNodes(snapshot.nodes);
    setEdges(snapshot.edges);
  };

  const undoWorkflowEdit = () => {
    const history = historyRef.current;
    if (!history.past.length) return;
    const current = JSON.parse(history.current) as CanvasHistorySnapshot;
    const previous = history.past.at(-1) as CanvasHistorySnapshot;
    history.past = history.past.slice(0, -1);
    history.future = [...history.future, current];
    restoreHistorySnapshot(previous);
    setStatus('Undid the last workflow edit. Save it to persist the change.');
  };

  const redoWorkflowEdit = () => {
    const history = historyRef.current;
    if (!history.future.length) return;
    const current = JSON.parse(history.current) as CanvasHistorySnapshot;
    const next = history.future.at(-1) as CanvasHistorySnapshot;
    history.future = history.future.slice(0, -1);
    history.past = [...history.past, current];
    restoreHistorySnapshot(next);
    setStatus('Redid the workflow edit. Save it to persist the change.');
  };

  const workflowDirty =
    workflowLoaded && savedWorkflowFingerprint !== null && currentWorkflowFingerprint !== savedWorkflowFingerprint;

  useEffect(() => {
    if (!client) return undefined;
    return client.subscribe((event) => {
      const eventKey = JSON.stringify(event);
      const now = Date.now();
      const previousEvent = recentEventRef.current.get(eventKey);
      if (previousEvent && now - previousEvent < 1000) return;
      recentEventRef.current.set(eventKey, now);
      for (const [key, timestamp] of recentEventRef.current)
        if (now - timestamp >= 1000) recentEventRef.current.delete(key);
      const projectId =
        typeof event.project === 'string'
          ? event.project
          : event.project &&
              typeof event.project === 'object' &&
              !Array.isArray(event.project) &&
              'session_id' in event.project
            ? String(event.project.session_id)
            : undefined;
      if (projectId && projectId !== project.session_id) return;
      const payload = event.payload;
      const detail =
        payload && typeof payload === 'object' && !Array.isArray(payload) && 'message' in payload
          ? String(payload.message)
          : undefined;
      const operation = event.operation_id ? ` (${event.operation_id})` : '';
      const level: CanvasLogLine['level'] = event.type.endsWith('.failed')
        ? 'error'
        : event.type.endsWith('.completed')
          ? 'success'
          : 'info';
      if (event.operation_id) {
        const executionState =
          event.type === 'operation.started'
            ? 'running'
            : event.type === 'operation.completed'
              ? 'completed'
              : event.type === 'operation.failed'
                ? 'failed'
                : undefined;
        if (executionState) {
          setNodes((current) =>
            current.map((node) => (node.id === event.operation_id ? { ...node, executionState } : node)),
          );
        }
      }
      appendLog(detail ? `${event.type}${operation}: ${detail}` : `${event.type}${operation}`, level);
      if (
        event.type === 'operation.completed' ||
        event.type === 'workflow.completed' ||
        event.type === 'workflow.failed'
      )
        void refreshArtifacts();
    });
  }, [appendLog, client, project.session_id, refreshArtifacts]);

  useEffect(() => {
    if (!client) return undefined;
    void refreshArtifacts();
    return undefined;
  }, [client, refreshArtifacts]);

  useEffect(() => {
    if (surface !== 'workflow' || !client) return undefined;
    let active = true;
    const poll = () =>
      client
        .workflowState(project.session_id)
        .then((result) => {
          if (active) {
            setWorkflowState(result.state);
            if (result.progress) setWorkflowProgress(result.progress);
            if (result.state === 'completed' || result.state === 'failed' || result.state === 'cancelled') {
              void refreshArtifacts();
              appendLog(
                `workflow ${result.state}: ${result.progress?.completed ?? 0}/${result.progress?.total ?? 0}`,
                result.state === 'failed' ? 'error' : result.state === 'completed' ? 'success' : 'info',
              );
            }
          }
        })
        .catch(() => undefined);
    void poll();
    const timer = window.setInterval(() => void poll(), 1000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [appendLog, client, project.session_id, refreshArtifacts, surface]);

  useEffect(() => {
    if (surface !== 'workflow' || !client || typeof client.workflowDefinition !== 'function') return undefined;
    let active = true;
    client
      .workflowDefinition(project.session_id)
      .then((result) => {
        if (!active) return;
        initialViewportFittedRef.current = false;
        setWorkflowRevision(result.workflow.version);
        setSavedWorkflowFingerprint(workflowFingerprint(result.workflow));
        if (!result.valid) {
          setStatus(`Saved workflow is invalid: ${result.diagnostics.map((item) => item.message).join('; ')}`);
          setWorkflowLoaded(true);
          return;
        }
        const loadedNodes = result.workflow.operations.map((operation, index) => {
          const capability = capabilities.operations.find((item) => item.canonical_id === operation.operation);
          const position =
            operation.position && Number.isFinite(operation.position.x) && Number.isFinite(operation.position.y)
              ? operation.position
              : workflowLayout(index);
          return {
            id: operation.id,
            kind: 'operation' as const,
            title: capability?.label || operation.operation,
            description: capability?.definition || '',
            x: position.x,
            y: position.y,
            outputsToCanvas: true,
            capabilityId: operation.operation,
            projectEntry: capability?.project_entry === true,
            parameters: uiParameters(capability, operation.parameters),
            executionState: 'idle' as const,
          };
        });
        const loadedEdges = result.workflow.connections.map((connection) => ({
          id: `${connection.source_operation}-${connection.source_port}-${connection.target_operation}-${connection.target_port}`,
          source: connection.source_operation,
          sourcePort: connection.source_port,
          target: connection.target_operation,
          targetPort: connection.target_port,
        }));
        nextId.current = loadedNodes.length + 1;
        setNodes(loadedNodes);
        setEdges(loadedEdges);
        setWorldSize((current) => ({
          width: Math.max(current.width, ...loadedNodes.map((node) => node.x + NODE_WIDTH + 800)),
          height: Math.max(current.height, ...loadedNodes.map((node) => node.y + NODE_HEIGHT + 600)),
        }));
        void refreshArtifacts();
        setWorkflowLoaded(true);
        setStatus('Workflow loaded from the project.');
      })
      .catch((error) => {
        if (active) {
          setWorkflowLoaded(true);
          setStatus(error instanceof Error ? error.message : 'Workflow could not be loaded.');
        }
      });
    return () => {
      active = false;
    };
  }, [capabilities.operations, client, project.session_id, refreshArtifacts, setStatus, surface]);

  useLayoutEffect(() => {
    if (!workflowLoaded || !nodes.length || initialViewportFittedRef.current) return undefined;
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const minX = Math.min(...nodes.map((node) => node.x));
    const minY = Math.min(...nodes.map((node) => node.y));
    const maxX = Math.max(...nodes.map((node) => node.x + NODE_WIDTH));
    const maxY = Math.max(...nodes.map((node) => node.y + NODE_HEIGHT));
    const padding = 80;
    const boundsWidth = Math.max(1, maxX - minX);
    const boundsHeight = Math.max(1, maxY - minY);
    const fittedScale = Math.max(
      0.35,
      Math.min(
        1.2,
        (canvas.clientWidth - padding * 2) / boundsWidth,
        (canvas.clientHeight - padding * 2) / boundsHeight,
      ),
    );
    initialViewportFittedRef.current = true;
    setScale(fittedScale);
    setOffset({
      x: (canvas.clientWidth - boundsWidth * fittedScale) / 2 - minX * fittedScale,
      y: (canvas.clientHeight - boundsHeight * fittedScale) / 2 - minY * fittedScale,
    });
    return undefined;
  }, [nodes, workflowLoaded]);

  const workflowAction = async (action: 'run' | 'pause' | 'cancel') => {
    if (!client) return;
    if (action === 'run' && workflowDirty) {
      setStatus('Save the workflow before running it.');
      return;
    }
    setWorkflowBusy(true);
    try {
      const result =
        action === 'run'
          ? await client.runWorkflow(project.session_id)
          : action === 'pause'
            ? await client.pauseWorkflow(project.session_id)
            : await client.cancelWorkflow(project.session_id);
      setWorkflowState(result.state);
      if (result.progress) setWorkflowProgress(result.progress);
      setStatus(`Workflow ${action} request accepted.`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : `Workflow ${action} failed.`);
    } finally {
      setWorkflowBusy(false);
    }
  };

  const validateCurrentWorkflow = async (): Promise<boolean> => {
    if (!client) return false;
    setWorkflowBusy(true);
    try {
      const result = await client.validateWorkflow(project.session_id, currentWorkflow);
      if (!result.valid) {
        setStatus(`Workflow is invalid: ${result.diagnostics.map((item) => item.message).join('; ')}`);
        setWorkflowState('failed');
        return false;
      }
      setWorkflowState('validated');
      setStatus('Workflow is valid for the current StreamFind installation.');
      return true;
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow validation failed.');
      return false;
    } finally {
      setWorkflowBusy(false);
    }
  };

  const saveCurrentWorkflow = async (): Promise<boolean> => {
    if (!client || !workflowLoaded) return false;
    setWorkflowBusy(true);
    try {
      const validation = await client.validateWorkflow(project.session_id, currentWorkflow);
      if (!validation.valid) {
        setStatus(`Workflow was not saved: ${validation.diagnostics.map((item) => item.message).join('; ')}`);
        return false;
      }
      const result = await client.saveWorkflow(project.session_id, currentWorkflow);
      setWorkflowRevision(result.workflow.version);
      setSavedWorkflowFingerprint(currentWorkflowFingerprint);

      setStatus(`Workflow saved (revision ${result.workflow.version}).`);
      return true;
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow could not be saved.');
      return false;
    } finally {
      setWorkflowBusy(false);
    }
  };

  const clearWorkflowHistory = async () => {
    if (!client || workflowBusy) return;
    setWorkflowBusy(true);
    try {
      await client.clearWorkflowHistory(project.session_id);
      historyRef.current = { past: [], current: historyRef.current.current, future: [] };
      setStatus('Workflow history and artifacts from prior revisions were cleared.');
      await refreshArtifacts();
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow history could not be cleared.');
    } finally {
      setWorkflowBusy(false);
    }
  };

  const resetWorkflow = () => {
    if (workflowBusy) return;
    nextId.current = 1;
    setNodes([]);
    setEdges([]);
    setExpandedParameters({});
    setSelectedNodeId(null);
    setSelectedEdgeId(null);
    setWorkflowState('idle');
    setWorkflowProgress({ completed: 0, total: 0, current_step: 0 });
    setStatus('Workflow reset to an empty definition. Save it if desired.');
  };

  const exportWorkflow = () => {
    const blob = new Blob([JSON.stringify(currentWorkflow, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = 'streamfind-workflow.json';
    link.click();
    URL.revokeObjectURL(url);
    setStatus('Workflow exported as streamfind-workflow.json.');
  };

  const applyImportedWorkflow = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file || !client) return;
    setWorkflowBusy(true);
    try {
      const parsed = JSON.parse(await file.text()) as Partial<WorkflowDefinition>;
      if (parsed.schema_version !== 1 || !Array.isArray(parsed.operations) || !Array.isArray(parsed.connections)) {
        throw new Error('The selected file is not a compatible StreamFind workflow schema.');
      }
      const imported: WorkflowDefinition = {
        schema_version: 1,
        workflow_id: parsed.workflow_id,
        name: parsed.name,
        version: workflowRevision,
        operations: parsed.operations,
        connections: parsed.connections,
      };
      const validation = await client.validateWorkflow(project.session_id, imported);
      if (!validation.valid) {
        setStatus(`Workflow was not loaded: ${validation.diagnostics.map((item) => item.message).join('; ')}`);
        return;
      }
      const loadedNodes = imported.operations.map((operation, index) => {
        const capability = capabilities.operations.find((item) => item.canonical_id === operation.operation);
        const position =
          operation.position && Number.isFinite(operation.position.x) && Number.isFinite(operation.position.y)
            ? operation.position
            : workflowLayout(index);
        return {
          id: operation.id,
          kind: 'operation' as const,
          title: capability?.label || operation.operation,
          description: capability?.definition || '',
          x: position.x,
          y: position.y,
          outputsToCanvas: true,
          capabilityId: operation.operation,
          projectEntry: capability?.project_entry === true,
          parameters: uiParameters(capability, operation.parameters),
          executionState: 'idle' as const,
        };
      });
      const loadedEdges = imported.connections.map((connection) => ({
        id: `${connection.source_operation}-${connection.source_port}-${connection.target_operation}-${connection.target_port}`,
        source: connection.source_operation,
        sourcePort: connection.source_port,
        target: connection.target_operation,
        targetPort: connection.target_port,
      }));
      nextId.current = loadedNodes.length + 1;
      setNodes(loadedNodes);
      setEdges(loadedEdges);
      setExpandedParameters({});
      setWorkflowState('validated');
      setStatus('Workflow imported. Save it to persist it in the project.');
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow could not be imported.');
    } finally {
      setWorkflowBusy(false);
    }
  };

  useEffect(() => {
    if (surface !== 'workflow' || !client || !['queued', 'running', 'cancelling'].includes(workflowState))
      return undefined;
    const timer = window.setInterval(() => {
      client
        .workflowState(project.session_id)
        .then((result) => {
          setWorkflowState(result.state);
          if (result.progress) setWorkflowProgress(result.progress);
          if (result.state !== 'idle') {
            appendLog(
              `workflow ${result.state}: ${result.progress?.completed ?? 0}/${result.progress?.total ?? 0}`,
              result.state === 'failed' ? 'error' : result.state === 'completed' ? 'success' : 'info',
            );
          }
        })
        .catch(() => undefined);
    }, 500);
    return () => window.clearInterval(timer);
  }, [appendLog, client, project.session_id, surface, workflowState]);

  const nodeMap = useMemo(() => new Map(nodes.map((node) => [node.id, node])), [nodes]);
  const toWorld = (event: ReactMouseEvent): Point => {
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return { x: (event.clientX - rect.left - offset.x) / scale, y: (event.clientY - rect.top - offset.y) / scale };
  };
  const nodeCapability = (node: CanvasNode) =>
    node.capabilityId ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId) : undefined;
  // This layout helper intentionally tracks the latest measured anchor positions.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const portPosition = (node: CanvasNode, portId: string, direction: 'input' | 'output'): Point => {
    const measured = anchorCenters[`${node.id}|${direction}|${portId}`];
    if (measured) return measured;
    const ports = nodePorts(nodeCapability(node))[direction === 'input' ? 'inputs' : 'outputs'];
    const capability = nodeCapability(node);
    if (direction === 'input' && portId.startsWith('parameter:')) {
      const parameterIndex = (capability?.parameters || [])
        .filter((parameter) => parameter.name !== 'database_path')
        .findIndex((parameter) => `parameter:${parameter.name}` === portId);
      const inputCount = ports.length;
      const outputCount = Math.max(1, nodePorts(capability).outputs.length);
      const parametersTop = 58 + (inputCount ? 25 + inputCount * 31 : 0) + 25 + outputCount * 31 + 54;
      const expanded = expandedParameters[node.id] ?? node.projectEntry;
      return {
        x: node.x - 12,
        y: node.y + parametersTop + (expanded ? Math.max(0, parameterIndex) * 38 + 8 : 15),
      };
    }
    const index = Math.max(
      0,
      ports.findIndex((port) => port.id === portId),
    );
    const top =
      direction === 'input'
        ? 94 + index * 31
        : (ports.length && nodePorts(capability).inputs.length ? 120 + nodePorts(capability).inputs.length * 31 : 94) +
          index * 31;
    return { x: node.x + (direction === 'input' ? -12 : NODE_WIDTH + 12), y: node.y + top };
  };
  const edgeTypeKey = (node: CanvasNode, portId: string, direction: 'input' | 'output'): string => {
    const capability = nodeCapability(node);
    if (direction === 'input' && portId.startsWith('parameter:')) {
      const parameter = capability?.parameters.find((candidate) => `parameter:${candidate.name}` === portId);
      return parameter ? parameterTypeKey(parameter) : 'unknown';
    }
    const port = nodePorts(capability)[direction === 'input' ? 'inputs' : 'outputs'].find(
      (candidate) => candidate.id === portId,
    );
    return port ? visualPortTypeKey(port) : 'unknown';
  };

  useEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const canvasRect = canvas.getBoundingClientRect();
      const next: Record<string, Point> = {};
      document.querySelectorAll<HTMLElement>('[data-canvas-anchor]').forEach((element) => {
        const key = element.dataset.canvasAnchor;
        if (!key) return;
        const rect = element.getBoundingClientRect();
        next[key] = {
          x: (rect.left + rect.width / 2 - canvasRect.left - offset.x) / scale,
          y: (rect.top + rect.height / 2 - canvasRect.top - offset.y) / scale,
        };
      });
      document.querySelectorAll<HTMLElement>('[data-canvas-parameter-summary]').forEach((element) => {
        const nodeId = element.dataset.canvasParameterSummary;
        const node = nodeId ? nodeMap.get(nodeId) : undefined;
        const capability = node?.capabilityId
          ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId)
          : undefined;
        if (!node || !capability) return;
        const rect = element.getBoundingClientRect();
        const point = {
          x: (rect.left + rect.width / 2 - canvasRect.left - offset.x) / scale,
          y: (rect.top + rect.height / 2 - canvasRect.top - offset.y) / scale,
        };
        capability.parameters
          .filter((parameter) => parameter.name !== 'database_path')
          .forEach((parameter) => {
            next[`${node.id}|input|parameter:${parameter.name}`] = point;
          });
      });
      setAnchorCenters(next);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [capabilities.operations, expandedParameters, nodeMap, nodes, offset, scale]);

  useEffect(() => {
    const move = (event: globalThis.MouseEvent) => {
      if (interaction?.type === 'pan') {
        setOffset({ x: event.clientX - (interaction.originX || 0), y: event.clientY - (interaction.originY || 0) });
        return;
      }
      if (interaction?.type === 'node' && interaction.id) {
        const rect = canvasRef.current?.getBoundingClientRect();
        if (!rect) return;
        let x = (event.clientX - rect.left - offset.x) / scale - (interaction.offsetX || 0);
        let y = (event.clientY - rect.top - offset.y) / scale - (interaction.offsetY || 0);
        if (gridVisible) {
          x = Math.round(x / GRID_SIZE) * GRID_SIZE;
          y = Math.round(y / GRID_SIZE) * GRID_SIZE;
        }
        setNodes((current) =>
          current.map((node) =>
            node.id === interaction.id ? { ...node, x: Math.max(12, x), y: Math.max(12, y) } : node,
          ),
        );
      }
      const pendingConnection = pendingConnectionRef.current;
      if (!connection && pendingConnection) {
        const movedX = event.clientX - pendingConnection.startX;
        const movedY = event.clientY - pendingConnection.startY;
        if (Math.hypot(movedX, movedY) >= 4) {
          const source = nodeMap.get(pendingConnection.source);
          if (source) {
            pendingConnectionRef.current = null;
            setConnection({
              source: pendingConnection.source,
              sourcePort: pendingConnection.sourcePort,
              point: portPosition(source, pendingConnection.sourcePort, 'output'),
            });
            setStatus('Drag this output to a compatible input on another operation.');
          }
        }
      }
      if (connection) {
        const rect = canvasRef.current?.getBoundingClientRect();
        if (rect)
          setConnection({
            ...connection,
            point: {
              x: (event.clientX - rect.left - offset.x) / scale,
              y: (event.clientY - rect.top - offset.y) / scale,
            },
          });
      }
    };
    const up = () => {
      pendingConnectionRef.current = null;
      setInteraction(null);
    };
    const blur = () => {
      setInteraction(null);
      setConnection(null);
    };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
    window.addEventListener('blur', blur);
    return () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
      window.removeEventListener('blur', blur);
    };
  }, [connection, gridVisible, interaction, nodeMap, offset, portPosition, scale, setStatus]);

  useEffect(() => {
    const centerProject = () => {
      const canvas = canvasRef.current;
      const entry = nodes[0];
      if (!canvas || !entry) return;
      setOffset({
        x: canvas.clientWidth / 2 - (entry.x + NODE_WIDTH / 2) * scale,
        y: canvas.clientHeight / 2 - (entry.y + NODE_HEIGHT / 2) * scale,
      });
    };
    const frame = window.requestAnimationFrame(centerProject);
    return () => window.cancelAnimationFrame(frame);
    // The initial project node is centered once; dragging must not recenter the canvas.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const handleWheel = (event: WheelEvent) => {
      if (
        (event.target as HTMLElement).closest(
          '.sf-canvas-toolbar, .sf-canvas-picker, .sf-canvas-status, .sf-node-info-pane, .sf-json-editor-overlay, .sf-artifact-viewer-backdrop',
        )
      )
        return;
      event.preventDefault();
      if (!event.ctrlKey && !event.metaKey) {
        setOffset((current) => ({ x: current.x - event.deltaX, y: current.y - event.deltaY }));
        return;
      }
      const rect = canvas.getBoundingClientRect();
      const cursorX = event.clientX - rect.left;
      const cursorY = event.clientY - rect.top;
      const { offset: currentOffset, scale: currentScale } = viewportRef.current;
      const worldX = (cursorX - currentOffset.x) / currentScale;
      const worldY = (cursorY - currentOffset.y) / currentScale;
      const nextScale = Math.min(2.8, Math.max(0.35, currentScale * Math.exp(-event.deltaY * 0.0015)));
      setScale(nextScale);
      setOffset({ x: cursorX - worldX * nextScale, y: cursorY - worldY * nextScale });
    };
    canvas.addEventListener('wheel', handleWheel, { passive: false });
    return () => canvas.removeEventListener('wheel', handleWheel);
  }, []);

  const beginPan = (event: ReactMouseEvent) => {
    const target = event.target as HTMLElement;
    if (
      (event.button !== 0 && event.button !== 1) ||
      target.closest('.sf-canvas-node, .sf-canvas-toolbar, .sf-canvas-picker')
    )
      return;
    setPicker(null);
    setInteraction({ type: 'pan', originX: event.clientX - offset.x, originY: event.clientY - offset.y });
  };
  const beginNodeDrag = (event: ReactMouseEvent, node: CanvasNode) => {
    event.stopPropagation();
    const point = toWorld(event);
    setPicker(null);
    setInteraction({ type: 'node', id: node.id, offsetX: point.x - node.x, offsetY: point.y - node.y });
  };
  const beginConnection = (event: ReactMouseEvent, node: CanvasNode, sourcePort: string) => {
    event.stopPropagation();
    setPicker(null);
    setConnection(null);
    pendingConnectionRef.current = { source: node.id, sourcePort, startX: event.clientX, startY: event.clientY };
  };
  const canConnect = (source: CanvasNode, target: CanvasNode, targetPort: string): boolean => {
    const sourcePorts = nodePorts(nodeCapability(source)).outputs;
    const targetPorts = nodePorts(nodeCapability(target)).inputs;
    const targetCapability = nodeCapability(target);
    const parameterName = targetPort.startsWith('parameter:') ? targetPort.slice('parameter:'.length) : '';
    const sourcePort = sourcePorts.find((port) => port.id === connection?.sourcePort);
    const targetInput = targetPorts.find((port) => port.id === targetPort);
    const targetParameter = targetCapability?.parameters.find((parameter) => parameter.name === parameterName);
    const compatibleTypes =
      sourcePort && targetInput
        ? sourcePort.typeKey !== 'unknown' &&
          targetInput.typeKey !== 'unknown' &&
          sourcePort.typeKey === targetInput.typeKey
        : false;
    const parameterCompatible =
      sourcePort && targetParameter ? portMatchesParameter(sourcePort, targetParameter) : false;
    return sourcePort !== undefined && (compatibleTypes || parameterCompatible);
  };
  const finishConnection = (event: ReactMouseEvent, target: CanvasNode, targetPort: string) => {
    event.stopPropagation();
    if (!connection || connection.source === target.id) return;
    const source = nodeMap.get(connection.source);
    if (!source || !canConnect(source, target, targetPort)) {
      setConnection(null);
      setStatus('Only operation output connectors can connect to compatible operation inputs.');
      return;
    }
    const edgeId = `${connection.source}-${connection.sourcePort}-${target.id}-${targetPort}`;
    setEdges((current) =>
      current.some((edge) => edge.id === edgeId)
        ? current
        : [
            ...current,
            {
              id: edgeId,
              source: connection.source,
              sourcePort: connection.sourcePort,
              target: target.id,
              targetPort,
            },
          ],
    );
    setConnection(null);
    setPicker(null);
    setStatus('Connected workflow nodes.');
  };
  const finishOnCanvas = (event: ReactMouseEvent) => {
    if (!connection) return;
    setPicker({ ...toWorld(event), source: connection.source, sourcePort: connection.sourcePort });
    setConnection(null);
  };
  const beginEdgeReconnect = (event: ReactMouseEvent, edge: CanvasEdge) => {
    event.stopPropagation();
    setSelectedEdgeId(edge.id);
    setEdges((current) => current.filter((candidate) => candidate.id !== edge.id));
    const source = nodeMap.get(edge.source);
    if (source) {
      setConnection({
        source: edge.source,
        sourcePort: edge.sourcePort,
        point: portPosition(source, edge.sourcePort, 'output'),
      });
      setStatus('Move the selected connector to a compatible input, or release on the canvas to choose a new node.');
    }
  };
  const deleteNode = (nodeId: string) => {
    setNodes((current) => current.filter((node) => node.id !== nodeId));
    setEdges((current) => current.filter((edge) => edge.source !== nodeId && edge.target !== nodeId));
    setSelectedNodeId(null);
    setStatus('Deleted node and its connected connectors.');
  };
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Delete' && event.key !== 'Backspace') return;
      const target = event.target as HTMLElement;
      if (target.matches('input, textarea, select, [contenteditable="true"]')) return;
      if (selectedEdgeId) {
        event.preventDefault();
        setEdges((current) => current.filter((edge) => edge.id !== selectedEdgeId));
        setSelectedEdgeId(null);
        setStatus('Deleted connector.');
      } else if (selectedNodeId) {
        event.preventDefault();
        setNodes((current) => current.filter((node) => node.id !== selectedNodeId));
        setEdges((current) =>
          current.filter((edge) => edge.source !== selectedNodeId && edge.target !== selectedNodeId),
        );
        setSelectedNodeId(null);
        setStatus('Deleted node and its connected connectors.');
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [selectedEdgeId, selectedNodeId, setStatus]);
  useEffect(() => {
    if (!selectedNodeId && !pickerCapabilityId) return undefined;
    const handleDocumentationKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      setSelectedNodeId(null);
      setPickerCapabilityId(null);
    };
    window.addEventListener('keydown', handleDocumentationKeyDown);
    return () => window.removeEventListener('keydown', handleDocumentationKeyDown);
  }, [pickerCapabilityId, selectedNodeId]);
  const addNode = (template: NodeTemplate) => {
    if (!picker) return;
    const id = `${template.kind}-${nextId.current++}`;
    const source = picker.source;
    const sourceNode = source ? nodeMap.get(source) : undefined;
    const position = sourceNode
      ? connectedNodePosition(sourceNode, nodes)
      : { x: picker.x - NODE_WIDTH / 2, y: picker.y - NODE_HEIGHT / 2 };
    const node: CanvasNode = {
      id,
      kind: template.kind,
      title: template.title,
      description: template.description,
      x: position.x,
      y: position.y,
      outputsToCanvas: template.outputsToCanvas,
      capabilityId: template.capabilityId,
      projectEntry: template.projectEntry,
      parameters: template.capabilityId
        ? defaultParameters(
            capabilities.operations.find((item) => item.canonical_id === template.capabilityId) as BackendCapability,
          )
        : {},
    };
    setNodes((current) => [...current, node]);
    if (source) {
      const targetCapability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      const sourcePort =
        sourceNode && nodePorts(nodeCapability(sourceNode)).outputs.find((port) => port.id === picker.sourcePort);
      const targetPort =
        nodePorts(targetCapability).inputs.find((port) => sourcePort && port.typeKey === sourcePort.typeKey)?.id ||
        (sourcePort
          ? targetCapability?.parameters.find(
              (parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter),
            )?.name
            ? `parameter:${targetCapability.parameters.find((parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter))?.name}`
            : ''
          : '');
      if (!targetPort) return;
      setEdges((current) => [
        ...current,
        { id: `${source}-${id}`, source, sourcePort: picker.sourcePort || '', target: id, targetPort },
      ]);
    }
    setPicker(null);
    setStatus(`Added ${template.title} node.`);
  };
  const availableTemplates = useMemo(() => {
    const standaloneTemplates = nodeTemplates;
    if (!picker?.source) return standaloneTemplates;
    const source = nodeMap.get(picker.source);
    if (!source) return standaloneTemplates;
    const sourceCapability = source.capabilityId
      ? capabilities.operations.find((item) => item.canonical_id === source.capabilityId)
      : undefined;
    const sourcePort = nodePorts(sourceCapability).outputs.find((port) => port.id === picker.sourcePort);
    return standaloneTemplates.filter((template) => {
      const targetCapability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      if (source.kind === 'method') return template.kind === 'method';
      if (source.kind === 'operation')
        return (
          (template.kind === 'operation' || template.kind === 'extract' || template.kind === 'plot') &&
          Boolean(
            sourcePort &&
            (nodePorts(targetCapability).inputs.some((port) => port.typeKey === sourcePort.typeKey) ||
              targetCapability?.parameters.some(
                (parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter),
              )),
          )
        );
      return false;
    });
  }, [capabilities.operations, nodeMap, nodeTemplates, picker]);
  const paletteModules = useMemo(
    () =>
      [
        ...new Set(
          availableTemplates.map((template) => template.module).filter((module): module is string => Boolean(module)),
        ),
      ].sort(),
    [availableTemplates],
  );
  const paletteSearchQuery = paletteSearch.trim().toLocaleLowerCase();
  const filteredTemplates = useMemo(() => {
    const categorized = availableTemplates.filter((template) => !paletteModule || template.module === paletteModule);
    if (!paletteSearchQuery) return categorized;
    return categorized.filter((template) => {
      const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      const ontologyFragment = [
        capability?.canonical_id,
        capability?.label,
        capability?.definition,
        capability?.domain,
        capability?.module_id,
      ]
        .filter((value): value is string => typeof value === 'string')
        .join(' ')
        .toLocaleLowerCase();
      return ontologyFragment.includes(paletteSearchQuery);
    });
  }, [availableTemplates, capabilities.operations, paletteModule, paletteSearchQuery]);
  const arrangeNodes = () => {
    if (!nodes.length) return;
    const indegree = new Map(nodes.map((node) => [node.id, 0]));
    const outgoing = new Map<string, string[]>();
    edges.forEach((edge) => {
      indegree.set(edge.target, (indegree.get(edge.target) || 0) + 1);
      outgoing.set(edge.source, [...(outgoing.get(edge.source) || []), edge.target]);
    });
    const queue = nodes.filter((node) => (indegree.get(node.id) || 0) === 0).map((node) => node.id);
    const depth = new Map(queue.map((id) => [id, 0]));
    for (let index = 0; index < queue.length; index += 1) {
      const current = queue[index];
      for (const next of outgoing.get(current) || []) {
        depth.set(next, Math.max(depth.get(next) || 0, (depth.get(current) || 0) + 1));
        const remaining = (indegree.get(next) || 0) - 1;
        indegree.set(next, remaining);
        if (remaining === 0) queue.push(next);
      }
    }
    nodes.forEach((node) => {
      if (!depth.has(node.id)) depth.set(node.id, 0);
    });
    const groups = new Map<number, CanvasNode[]>();
    nodes.forEach((node) =>
      groups.set(depth.get(node.id) || 0, [...(groups.get(depth.get(node.id) || 0) || []), node]),
    );
    const arranged = nodes.map((node) => ({ ...node }));
    const byId = new Map(arranged.map((node) => [node.id, node]));
    const stages = Array.from(groups.keys()).sort((a, b) => a - b);
    const maxStage = stages[stages.length - 1] || 0;
    const largestColumn = Math.max(...stages.map((stage) => groups.get(stage)?.length || 0), 1);
    const layoutWidth = Math.max(CANVAS_WORLD_WIDTH, (maxStage + 1) * 360 + NODE_WIDTH + 800);
    const layoutHeight = Math.max(CANVAS_WORLD_HEIGHT, largestColumn * 190 + 600);
    const centerX = layoutWidth / 2;
    const centerY = layoutHeight / 2;
    stages.forEach((stage) => {
      const column = groups.get(stage) || [];
      column.sort((a, b) => a.y - b.y || a.id.localeCompare(b.id));
      column.forEach((node, row) => {
        const target = byId.get(node.id);
        if (!target) return;
        target.x = centerX + (stage - maxStage / 2) * 360 - NODE_WIDTH / 2;
        target.y = centerY + (row - (column.length - 1) / 2) * 190 - NODE_HEIGHT / 2;
      });
    });
    setWorldSize((current) => ({
      width: Math.max(current.width, layoutWidth),
      height: Math.max(current.height, layoutHeight),
    }));
    setNodes(arranged);
    setScale(1);
    const minX = Math.min(...arranged.map((node) => node.x));
    const minY = Math.min(...arranged.map((node) => node.y));
    const maxX = Math.max(...arranged.map((node) => node.x + NODE_WIDTH));
    const maxY = Math.max(...arranged.map((node) => node.y + NODE_HEIGHT));
    const canvas = canvasRef.current;
    if (canvas)
      setOffset({ x: canvas.clientWidth / 2 - (minX + maxX) / 2, y: canvas.clientHeight / 2 - (minY + maxY) / 2 });
    setStatus('Arranged nodes by dependency stage from left to right.');
  };
  const zoom = (amount: number) =>
    setScale((current) => Math.min(2.8, Math.max(0.35, Number((current + amount).toFixed(2)))));
  const openStandalonePicker = () => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    setPicker({
      x: (canvas.clientWidth / 2 - offset.x) / scale,
      y: (canvas.clientHeight / 2 - offset.y) / scale,
    });
  };
  const updateNodeParameter = (nodeId: string, parameterName: string, value: unknown) => {
    setNodes((current) =>
      current.map((node) =>
        node.id === nodeId ? { ...node, parameters: { ...(node.parameters || {}), [parameterName]: value } } : node,
      ),
    );
  };
  const openJsonEditor = (node: CanvasNode, parameter: CapabilityParameter) => {
    setJsonEditor({ nodeId: node.id, parameter });
    setJsonEditorText(
      JSON.stringify(
        node.parameters?.[parameter.name] ??
          (parameter.schema.type === 'array' || parameter.schema.type === 'table' ? [] : {}),
        null,
        2,
      ),
    );
    setJsonEditorError(null);
  };
  const openPathWizard = (node: CanvasNode, parameter: CapabilityParameter, mergeIntoJsonEditor = false) => {
    setPathWizard({ nodeId: node.id, parameter, mergeIntoJsonEditor });
  };
  const openPathWizardFromJsonEditor = () => {
    if (!jsonEditor) return;
    const node = nodes.find((candidate) => candidate.id === jsonEditor.nodeId);
    if (node) openPathWizard(node, jsonEditor.parameter, true);
  };
  const saveJsonEditor = () => {
    if (!jsonEditor) return;
    try {
      const parsed = JSON.parse(jsonEditorText);
      const validationError = validateJsonValue(parsed, jsonEditor.parameter.schema);
      if (validationError) {
        setJsonEditorError(validationError);
        return;
      }
      updateNodeParameter(jsonEditor.nodeId, jsonEditor.parameter.name, parsed);
      setJsonEditor(null);
      setJsonEditorError(null);
    } catch (error) {
      setJsonEditorError(error instanceof Error ? error.message : 'Invalid JSON.');
    }
  };
  const importJsonCsv = async (file: File) => {
    if (!jsonEditor) return;
    const schema = jsonEditor.parameter.schema;
    const itemSchema = schema.items;
    const rowSchema = schema.properties ? schema : itemSchema;
    const columns = rowSchema?.properties ? schemaPropertyOrder(rowSchema) : [];
    if (!columns.length) {
      setJsonEditorError('CSV import requires an array of structured rows with declared columns.');
      return;
    }
    const parsed = parseCsvRows(await file.text(), columns);
    if (parsed.error || !parsed.rows) {
      setJsonEditorError(parsed.error || 'CSV could not be parsed.');
      return;
    }
    setJsonEditorText(JSON.stringify(parsed.rows, null, 2));
    setJsonEditorError(null);
  };
  const addSelectedPaths = async (node: CanvasNode, parameter: CapabilityParameter, folders: boolean) => {
    if (!client) return;
    try {
      const paths = folders ? await client.pickFolders() : await client.pickFiles(fileParameterExtensions(parameter));
      const current: unknown[] = Array.isArray(node.parameters?.[parameter.name])
        ? (node.parameters[parameter.name] as unknown[])
        : [];
      const existing = new Set(
        current.map((item) =>
          typeof item === 'string'
            ? item
            : typeof item === 'object' && item !== null
              ? String((item as Record<string, unknown>).path || '')
              : '',
        ),
      );
      const added = paths
        .filter((path) => !existing.has(path))
        .map((path) => (isSimplePathParameter(parameter) ? path : { path }));
      updateNodeParameter(node.id, parameter.name, [...current, ...added]);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const openTableEditorFromJsonEditor = () => {
    if (!jsonEditor) return;
    try {
      const parsed = JSON.parse(jsonEditorText);
      if (!Array.isArray(parsed)) throw new Error('Table JSON must be an array of rows.');
      const rows = parsed.filter(
        (row): row is Record<string, unknown> => typeof row === 'object' && row !== null && !Array.isArray(row),
      );
      if (rows.length !== parsed.length) throw new Error('Every table row must be a JSON object.');
      setTableEditor({
        nodeId: jsonEditor.nodeId,
        parameter: jsonEditor.parameter,
        rows,
        returnToJsonEditor: true,
        error: null,
      });
      setJsonEditor(null);
    } catch (error) {
      setJsonEditorError(error instanceof Error ? error.message : 'Invalid table JSON.');
    }
  };
  const updateTableCell = (rowIndex: number, columnName: string, value: string) => {
    setTableEditor((current) => {
      if (!current) return current;
      const rows = current.rows.map((row, index) => (index === rowIndex ? { ...row, [columnName]: value } : row));
      return { ...current, rows, error: null };
    });
  };
  const chooseTableColumnPaths = async (columnName: string, schema: JsonSchema, folders = false) => {
    if (!client || !tableEditor) return;
    const declared = schema.extensions;
    const extensions = Array.isArray(declared)
      ? declared.filter((item): item is string => typeof item === 'string')
      : [];
    try {
      const paths = folders ? await client.pickFolders() : await client.pickFiles(extensions);
      const columns = schemaPropertyOrder(tableEditor.parameter.schema);
      const rows = [
        ...tableEditor.rows,
        ...paths.map(
          (path) =>
            Object.fromEntries(columns.map((column) => [column, column === columnName ? path : ''])) as Record<
              string,
              unknown
            >,
        ),
      ];
      setTableEditor({ ...tableEditor, rows });
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const saveTableEditor = () => {
    if (!tableEditor) return;
    const error = validateInlineTableRows(tableEditor.parameter, tableEditor.rows);
    if (error) {
      setTableEditor((current) => (current ? { ...current, error } : current));
      return;
    }
    const normalized = normalizeInlineTableRows(tableEditor.parameter, tableEditor.rows);
    if (tableEditor.returnToJsonEditor) {
      setJsonEditor({ nodeId: tableEditor.nodeId, parameter: tableEditor.parameter });
      setJsonEditorText(JSON.stringify(normalized, null, 2));
      setJsonEditorError(null);
      setTableEditor(null);
      return;
    }
    updateNodeParameter(tableEditor.nodeId, tableEditor.parameter.name, normalized);
    setTableEditor(null);
  };
  const importTableCsv = async (file: File) => {
    if (!tableEditor) return;
    const parsed = parseCsvRows(await file.text(), schemaPropertyOrder(tableEditor.parameter.schema));
    if (parsed.error || !parsed.rows) {
      setCsvPreview({ rows: [], error: parsed.error || 'CSV could not be parsed.' });
      return;
    }
    const validation = validateInlineTableRows(tableEditor.parameter, parsed.rows);
    setCsvPreview({ rows: parsed.rows, error: validation || undefined });
  };
  const runEntryOperation = async (node: CanvasNode, capability: BackendCapability) => {
    if (!client || !node.capabilityId) return;
    if (workflowDirty) {
      const saved = await saveCurrentWorkflow();
      if (!saved) return;
    }
    setNodes((current) => current.map((item) => (item.id === node.id ? { ...item, executionState: 'running' } : item)));
    try {
      const result = await client.runOperation(
        project.session_id,
        node.capabilityId,
        wireParameters(capability, node.parameters || {}),
        node.id,
      );
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id
            ? { ...item, executionState: 'completed', executionMessage: JSON.stringify(result) }
            : item,
        ),
      );
      const artifacts = await refreshArtifacts();
      const visualization = artifacts.find(
        (artifact) => artifact.producer_instance === node.id && isVisualizationArtifact(artifact),
      );
      if (visualization) {
        setArtifactViewer(visualization);
        setArtifactData(null);
        setArtifactViewerSearch('');
        setArtifactViewerSort('');
        setArtifactViewerDescending(false);
      }
      const cacheHit =
        result !== null && typeof result === 'object' && !Array.isArray(result) && result.cache_hit === true;
      setStatus(
        cacheHit ? `${capability.label} reused cached artifacts; execution skipped.` : `${capability.label} completed.`,
      );
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Operation failed.';
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id ? { ...item, executionState: 'failed', executionMessage: message } : item,
        ),
      );
      setStatus(message);
    }
  };
  const stopOperation = async (node: CanvasNode) => {
    if (!client || node.executionState !== 'running') return;
    try {
      await client.cancelWorkflow(project.session_id);
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id
            ? { ...item, executionState: 'idle', executionMessage: 'Operation cancellation requested.' }
            : item,
        ),
      );
      setStatus('Operation cancellation requested.');
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Operation cancellation failed.');
    }
  };
  const selectedNode = selectedNodeId ? nodeMap.get(selectedNodeId) : undefined;
  const selectedCapability = selectedNode ? nodeCapability(selectedNode) : undefined;
  const pickerCapability = pickerCapabilityId
    ? capabilities.operations.find((item) => item.canonical_id === pickerCapabilityId)
    : undefined;
  const documentationCapability = selectedCapability || pickerCapability;
  useEffect(() => {
    if (!documentationFocus) return;
    const frame = window.requestAnimationFrame(() => {
      window.setTimeout(() => {
        const target = Array.from(document.querySelectorAll<HTMLElement>('[data-ontology-focus]')).find(
          (element) =>
            element.dataset.ontologyFocusSection === documentationFocus.section &&
            element.dataset.ontologyFocusKey === documentationFocus.key,
        );
        if (target) {
          const pane = target.closest<HTMLElement>('.sf-node-info-pane');
          if (pane) {
            pane.scrollTo({
              top: Math.max(0, target.offsetTop - pane.clientHeight / 2 + target.offsetHeight / 2),
              behavior: 'smooth',
            });
          }
          target.focus({ preventScroll: true });
        }
      }, 0);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [documentationFocus, documentationCapability]);
  const openNodeDocumentation = (nodeId: string, focus: DocumentationFocus | null = null) => {
    setDocumentationFocus(focus);
    setSelectedNodeId(nodeId);
  };
  const pathWizardNode = pathWizard ? nodes.find((node) => node.id === pathWizard.nodeId) : undefined;
  const pathWizardEntries =
    pathWizardNode && pathWizard
      ? Array.isArray(pathWizardNode.parameters?.[pathWizard.parameter.name])
        ? (pathWizardNode.parameters[pathWizard.parameter.name] as unknown[]).filter(
            (path): path is string => typeof path === 'string',
          )
        : []
      : [];
  const updatePathWizardSelection = (paths: string[]) => {
    if (!pathWizard || !pathWizardNode) return;
    if (pathWizard.mergeIntoJsonEditor && jsonEditor) {
      let current: unknown[] = [];
      try {
        const parsed = JSON.parse(jsonEditorText);
        if (Array.isArray(parsed)) current = parsed;
      } catch {
        current = pathWizardEntries;
      }
      const merged = [...current];
      for (const path of paths) if (!merged.includes(path)) merged.push(path);
      updateNodeParameter(pathWizardNode.id, pathWizard.parameter.name, merged);
      setJsonEditorText(JSON.stringify(merged, null, 2));
      setJsonEditorError(null);
      return;
    }
    updateNodeParameter(pathWizardNode.id, pathWizard.parameter.name, paths);
  };

  return (
    <div
      ref={canvasRef}
      className={`sf-canvas sf-canvas-shell ${gridVisible ? 'grid-visible' : ''} ${interaction?.type === 'pan' ? 'panning' : ''}`}
      data-canvas-surface={surface}
      onMouseDown={beginPan}
      onMouseUp={finishOnCanvas}
    >
      <div className="sf-canvas-toolbar" onMouseDown={(event) => event.stopPropagation()}>
        {onProjectHub ? (
          <button
            type="button"
            className="sf-canvas-control sf-home-button"
            onClick={onProjectHub}
            aria-label="Project Hub"
            title="Project Hub"
          >
            <i className="fa-solid fa-house" />
          </button>
        ) : null}
        <button type="button" className="sf-canvas-control" onClick={() => zoom(0.1)} title="Zoom in">
          <i className="fa-solid fa-plus" />
        </button>
        <span className="sf-canvas-zoom">{Math.round(scale * 100)}%</span>
        <button type="button" className="sf-canvas-control" onClick={() => zoom(-0.1)} title="Zoom out">
          <i className="fa-solid fa-minus" />
        </button>
        <button type="button" className="sf-canvas-control" onClick={arrangeNodes} title="Reset view and arrange nodes">
          <i className="fa-solid fa-crosshairs" />
        </button>
        <button
          type="button"
          className={`sf-canvas-control ${gridVisible ? 'active' : ''}`}
          onClick={() => setGridVisible((value) => !value)}
          title="Toggle grid"
        >
          <i className="fa-solid fa-grip" />
        </button>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={openStandalonePicker}
          title="Add Operation"
          aria-label="Add Operation"
        >
          <i className="fa-solid fa-diagram-project" />
        </button>
        {surface === 'workflow' && client ? (
          <>
            <input
              ref={workflowFileInputRef}
              type="file"
              accept="application/json,.json"
              onChange={(event) => void applyImportedWorkflow(event)}
              hidden
            />
            <button
              type="button"
              className={`sf-canvas-control ${workflowDirty ? 'unsaved' : ''}`}
              onClick={() => void saveCurrentWorkflow()}
              disabled={workflowBusy || !workflowLoaded || !workflowDirty}
              title={workflowDirty ? 'Save workflow changes' : 'Workflow is saved'}
              aria-label="Save workflow"
            >
              <i className="fa-solid fa-floppy-disk" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={resetWorkflow}
              disabled={workflowBusy}
              title="Reset workflow to empty"
              aria-label="Reset workflow to empty"
            >
              <i className="fa-solid fa-broom" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={undoWorkflowEdit}
              title="Undo workflow edit"
              aria-label="Undo workflow edit"
            >
              <i className="fa-solid fa-arrow-left" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={redoWorkflowEdit}
              title="Redo workflow edit"
              aria-label="Redo workflow edit"
            >
              <i className="fa-solid fa-arrow-right" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void clearWorkflowHistory()}
              title="Clear saved workflow history and old artifacts"
              aria-label="Clear saved workflow history and old artifacts"
            >
              <i className="fa-solid fa-trash-can" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={exportWorkflow}
              disabled={workflowBusy}
              title="Export workflow JSON"
              aria-label="Export workflow JSON"
            >
              <i className="fa-solid fa-file-export" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => workflowFileInputRef.current?.click()}
              disabled={workflowBusy}
              title="Load workflow JSON"
              aria-label="Load workflow JSON"
            >
              <i className="fa-solid fa-file-import" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void validateCurrentWorkflow()}
              disabled={workflowBusy}
              title="Validate workflow"
            >
              <i className="fa-solid fa-check" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('run')}
              disabled={workflowBusy}
              title="Run workflow"
            >
              <i className="fa-solid fa-play" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('pause')}
              disabled={workflowBusy}
              title="Pause workflow"
            >
              <i className="fa-solid fa-pause" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('cancel')}
              disabled={workflowBusy}
              title="Cancel workflow"
            >
              <i className="fa-solid fa-stop" />
            </button>
          </>
        ) : null}
      </div>
      {surface === 'workflow' && workflowLoaded && nodes.length === 0 ? (
        <div className="sf-empty-workflow-prompt" onMouseDown={(event) => event.stopPropagation()}>
          <button type="button" onClick={openStandalonePicker} aria-label="Add operation">
            <i className="fa-solid fa-diagram-project" />
            <span>Add operation</span>
          </button>
        </div>
      ) : null}
      <div
        className="sf-canvas-status"
        role="log"
        aria-live="polite"
        aria-label="Canvas activity log"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="sf-canvas-status-heading">
          <strong>{surface === 'workflow' ? 'Workflow terminal' : 'Explorer terminal'}</strong>
        </div>
        <div className="sf-canvas-status-current">{status}</div>
        <div className="sf-canvas-status-feed">
          {activityLog.map((entry) => (
            <div className={`sf-canvas-status-line ${entry.level}`} key={entry.id}>
              <time>{entry.timestamp}</time>
              <span>{entry.message}</span>
            </div>
          ))}
        </div>
      </div>
      <div
        className="sf-canvas-world"
        style={{
          width: canvasWorldSize.width,
          height: canvasWorldSize.height,
          transform: `translate(${offset.x}px, ${offset.y}px) scale(${scale})`,
        }}
      >
        <svg
          className="sf-canvas-edges"
          width={canvasWorldSize.width}
          height={canvasWorldSize.height}
          aria-hidden="true"
        >
          {edges.map((edge) => {
            const source = nodeMap.get(edge.source);
            const target = nodeMap.get(edge.target);
            if (!source || !target) return null;
            const from = portPosition(source, edge.sourcePort, 'output');
            const to = portPosition(target, edge.targetPort, 'input');
            const bend = Math.max(70, Math.abs(to.x - from.x) * 0.45);
            return (
              <path
                key={edge.id}
                className={`${typeClass(edgeTypeKey(source, edge.sourcePort, 'output'))}${selectedEdgeId === edge.id ? ' selected' : ''}`}
                d={`M ${from.x} ${from.y} C ${from.x + bend} ${from.y}, ${to.x - bend} ${to.y}, ${to.x} ${to.y}`}
                onMouseDown={(event) => beginEdgeReconnect(event, edge)}
                onClick={(event) => {
                  event.stopPropagation();
                  setSelectedEdgeId(edge.id);
                  setSelectedNodeId(null);
                }}
              />
            );
          })}
          {connection
            ? (() => {
                const source = nodeMap.get(connection.source);
                if (!source) return null;
                const from = portPosition(source, connection.sourcePort, 'output');
                const bend = Math.max(70, Math.abs(connection.point.x - from.x) * 0.45);
                return (
                  <path
                    className={`pending ${typeClass(edgeTypeKey(source, connection.sourcePort, 'output'))}`}
                    d={`M ${from.x} ${from.y} C ${from.x + bend} ${from.y}, ${connection.point.x - bend} ${connection.point.y}, ${connection.point.x} ${connection.point.y}`}
                  />
                );
              })()
            : null}
        </svg>
        {nodes.map((node) => {
          const capability = node.capabilityId
            ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId)
            : undefined;
          const domainClass = (capability?.domain || 'core').toLowerCase().replace(/[^a-z0-9]+/g, '-');
          const outputArtifact = (port: NodePort) =>
            currentArtifacts
              .filter(
                (artifact) =>
                  artifact.status === 'published' &&
                  artifactContractMatches(artifact.contract_id, port.semanticContract) &&
                  artifact.producer_instance === node.id,
              )
              .at(-1);
          return (
            <div
              key={node.id}
              className={`sf-canvas-node ${node.kind} sf-domain-${domainClass} ${node.executionState === 'running' ? 'running' : ''}`}
              style={{ left: node.x, top: node.y }}
              onMouseDown={(event) => beginNodeDrag(event, node)}
              onMouseUp={() => {
                setInteraction(null);
              }}
            >
              <div className="sf-canvas-node-header">
                <div className="sf-node-header-actions">
                  <button
                    type="button"
                    className="sf-node-info"
                    aria-label={`Open information for ${node.title}`}
                    title="Operation documentation"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      setDocumentationFocus(null);
                      setSelectedNodeId(node.id);
                    }}
                  >
                    <i className="fa-solid fa-circle-info" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-run"
                    aria-label={`Run ${node.title}`}
                    title="Run operation"
                    disabled={!capability || node.executionState === 'running'}
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      if (capability) void runEntryOperation(node, capability);
                    }}
                  >
                    <i className="fa-solid fa-play" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-stop"
                    aria-label={`Stop ${node.title}`}
                    title="Stop operation"
                    disabled={node.executionState !== 'running'}
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      void stopOperation(node);
                    }}
                  >
                    <i className="fa-solid fa-stop" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-delete"
                    aria-label={`Delete ${node.title}`}
                    title="Delete node and connected connectors"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      deleteNode(node.id);
                    }}
                  >
                    <i className="fa-solid fa-trash" />
                  </button>
                </div>
                <strong className="sf-node-title">{node.title}</strong>
                {capability?.module_id ? <span>{capability.module_id}</span> : null}
              </div>
              {nodePorts(capability).inputs.length ? (
                <section className="sf-node-section sf-node-inputs">
                  <h4>Inputs</h4>
                  {nodePorts(capability).inputs.map((port) => (
                    <div className="sf-node-port-row" key={port.id} title={port.description}>
                      <button
                        type="button"
                        className={`sf-node-port input ${typeClass(visualPortTypeKey(port))}`}
                        data-canvas-anchor={`${node.id}|input|${port.id}`}
                        aria-label={`Connect to ${node.title} input ${port.label}`}
                        onMouseDown={(event) => event.stopPropagation()}
                        onMouseUp={(event) => finishConnection(event, node, port.id)}
                      >
                        <i className={typeIcon(visualPortTypeKey(port))} aria-hidden="true" />
                      </button>
                      <button
                        type="button"
                        className="sf-node-ontology-link"
                        onMouseDown={(event) => event.stopPropagation()}
                        onClick={(event) => {
                          event.stopPropagation();
                          openNodeDocumentation(node.id, { section: 'inputs', key: port.id });
                        }}
                      >
                        {port.label}
                        {port.required ? ' *' : ''}
                      </button>
                    </div>
                  ))}
                </section>
              ) : null}
              <section className="sf-node-section sf-node-outputs">
                <h4>Outputs</h4>
                {nodePorts(capability).outputs.length ? (
                  nodePorts(capability).outputs.map((port) => (
                    <div
                      className={`sf-node-port-row output ${outputArtifact(port) ? 'has-artifact' : ''}`}
                      key={port.id}
                      title={port.description}
                    >
                      <button
                        type="button"
                        className="sf-node-ontology-link"
                        onMouseDown={(event) => event.stopPropagation()}
                        onClick={(event) => {
                          event.stopPropagation();
                          openNodeDocumentation(node.id, { section: 'outputs', key: port.id });
                        }}
                      >
                        {port.label}
                      </button>
                      <button
                        type="button"
                        className={`sf-node-port output ${typeClass(visualPortTypeKey(port))} ${outputArtifact(port) ? 'available' : ''}`}
                        data-canvas-anchor={`${node.id}|output|${port.id}`}
                        aria-label={`Connect from ${node.title} output ${port.label}`}
                        onMouseDown={(event) => beginConnection(event, node, port.id)}
                        onMouseUp={(event) => event.stopPropagation()}
                        onDoubleClick={(event) => {
                          event.stopPropagation();
                          pendingConnectionRef.current = null;
                          setConnection(null);
                          const artifact = outputArtifact(port);
                          if (artifact) {
                            setArtifactViewer(artifact as ArtifactRecord);
                            setArtifactViewerSearch('');
                            setArtifactViewerSort('');
                            setArtifactViewerDescending(false);
                          }
                        }}
                        onClick={(event) => {
                          if (event.detail !== 2) return;
                          event.stopPropagation();
                          pendingConnectionRef.current = null;
                          setConnection(null);
                          const artifact = outputArtifact(port);
                          if (artifact) {
                            setArtifactViewer(artifact as ArtifactRecord);
                            setArtifactData(null);
                            setArtifactViewerSearch('');
                            setArtifactViewerSort('');
                            setArtifactViewerDescending(false);
                          }
                        }}
                      >
                        <i className={typeIcon(visualPortTypeKey(port))} aria-hidden="true" />
                      </button>
                      {outputArtifact(port) ? (
                        <div className="sf-output-artifact-popover" role="tooltip">
                          <strong>{port.label}</strong>
                          <span>{artifactSummary(outputArtifact(port) as ArtifactRecord)}</span>
                          <small>Double-click the anchor to open this artifact</small>
                        </div>
                      ) : null}
                    </div>
                  ))
                ) : (
                  <div className="sf-node-empty">No declared outputs</div>
                )}
              </section>
              {capability && capability.parameters.some((parameter) => parameter.name !== 'database_path') ? (
                <>
                  <button
                    type="button"
                    className="sf-node-section-toggle"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={() => setExpandedParameters((current) => ({ ...current, [node.id]: !current[node.id] }))}
                  >
                    <span>Parameters</span>
                    {!(expandedParameters[node.id] ?? node.projectEntry) ? (
                      <span
                        className="sf-node-parameter-summary-port"
                        data-canvas-parameter-summary={node.id}
                        title="Parameter connectors; expand Parameters to edit individual connections"
                        aria-label="Collapsed parameter connectors"
                      >
                        <i className="fa-solid fa-sliders" aria-hidden="true" />
                      </span>
                    ) : null}
                    <i
                      className={`fa-solid fa-chevron-${(expandedParameters[node.id] ?? node.projectEntry) ? 'up' : 'down'}`}
                    />
                  </button>
                  {(expandedParameters[node.id] ?? node.projectEntry) ? (
                    <div className="sf-canvas-node-parameters" onMouseDown={(event) => event.stopPropagation()}>
                      {capability.parameters
                        .filter((parameter) => parameter.name !== 'database_path')
                        .map((parameter) => {
                          const value = node.parameters?.[parameter.name];
                          if (isTableParameter(parameter)) {
                            return (
                              <div className="sf-canvas-parameter" key={parameter.name}>
                                <button
                                  type="button"
                                  className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                                  data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                  aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                                >
                                  <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                                </button>
                                <div className="sf-canvas-parameter-heading">
                                  <button
                                    type="button"
                                    className="sf-node-ontology-link"
                                    onMouseDown={(event) => event.stopPropagation()}
                                    onClick={(event) => {
                                      event.stopPropagation();
                                      openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                    }}
                                  >
                                    {parameter.label || parameter.name}
                                  </button>
                                </div>
                                <div className="sf-json-parameter-actions">
                                  <button
                                    type="button"
                                    className="sf-button secondary"
                                    onClick={() => openJsonEditor(node, parameter)}
                                  >
                                    <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                  </button>
                                </div>
                              </div>
                            );
                          }
                          if (isFileListParameter(parameter)) {
                            const entries = Array.isArray(value) ? value : [];
                            return (
                              <div className="sf-canvas-parameter" key={parameter.name}>
                                <button
                                  type="button"
                                  className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                                  data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                  aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                                >
                                  <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                                </button>
                                <div className="sf-canvas-parameter-heading">
                                  <button
                                    type="button"
                                    className="sf-node-ontology-link"
                                    onMouseDown={(event) => event.stopPropagation()}
                                    onClick={(event) => {
                                      event.stopPropagation();
                                      openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                    }}
                                  >
                                    {parameter.label || parameter.name}
                                  </button>
                                  <span>
                                    {parameter.extensions?.length ? `.${parameter.extensions.join(', .')}` : ''}
                                    {parameter.directory_extensions?.length
                                      ? ` · folders: .${parameter.directory_extensions.join(', .')}`
                                      : ''}
                                  </span>
                                </div>
                                <div className="sf-canvas-parameter-actions">
                                  <button
                                    type="button"
                                    className="sf-button secondary"
                                    onClick={() => openJsonEditor(node, parameter)}
                                  >
                                    <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                  </button>
                                  <button
                                    type="button"
                                    className="sf-button secondary"
                                    onClick={() => void addSelectedPaths(node, parameter, false)}
                                  >
                                    <i className="fa-solid fa-file" /> Choose files
                                  </button>
                                  <button
                                    type="button"
                                    className="sf-button secondary"
                                    onClick={() => void addSelectedPaths(node, parameter, true)}
                                  >
                                    <i className="fa-solid fa-folder" /> Choose folders
                                  </button>
                                </div>
                                <small className="sf-canvas-file-summary">
                                  {entries.length
                                    ? `${entries.length} path${entries.length === 1 ? '' : 's'} selected`
                                    : 'No paths selected'}
                                </small>
                                {entries.map((entry, index) => {
                                  const isSimplePath = isSimplePathParameter(parameter);
                                  const record =
                                    typeof entry === 'object' && entry !== null
                                      ? (entry as Record<string, unknown>)
                                      : {};
                                  const pathValue = typeof entry === 'string' ? entry : String(record.path || '');
                                  return (
                                    <div className="sf-canvas-file-entry" key={`${pathValue || 'new'}-${index}`}>
                                      <input
                                        aria-label={`Path ${index + 1}`}
                                        value={pathValue}
                                        placeholder="Path"
                                        onChange={(event) => {
                                          const next = entries.map((item, itemIndex) =>
                                            itemIndex === index
                                              ? isSimplePath
                                                ? event.target.value
                                                : { ...record, path: event.target.value }
                                              : item,
                                          );
                                          updateNodeParameter(node.id, parameter.name, next);
                                        }}
                                      />
                                      {!isSimplePath ? (
                                        <>
                                          <input
                                            aria-label={`Replicate ${index + 1}`}
                                            value={String(record.replicate_name || '')}
                                            placeholder="Replicate"
                                            onChange={(event) => {
                                              const next = entries.map((item, itemIndex) =>
                                                itemIndex === index
                                                  ? { ...record, replicate_name: event.target.value }
                                                  : item,
                                              );
                                              updateNodeParameter(node.id, parameter.name, next);
                                            }}
                                          />
                                          <input
                                            aria-label={`Blank ${index + 1}`}
                                            value={String(record.blank_name || '')}
                                            placeholder="Blank"
                                            onChange={(event) => {
                                              const next = entries.map((item, itemIndex) =>
                                                itemIndex === index
                                                  ? { ...record, blank_name: event.target.value }
                                                  : item,
                                              );
                                              updateNodeParameter(node.id, parameter.name, next);
                                            }}
                                          />
                                        </>
                                      ) : null}
                                      <button
                                        type="button"
                                        className="sf-button secondary"
                                        onClick={() =>
                                          updateNodeParameter(
                                            node.id,
                                            parameter.name,
                                            entries.filter((_, itemIndex) => itemIndex !== index),
                                          )
                                        }
                                      >
                                        Remove
                                      </button>
                                    </div>
                                  );
                                })}
                              </div>
                            );
                          }
                          return (
                            <div className="sf-canvas-parameter" key={parameter.name}>
                              <button
                                type="button"
                                className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                                data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                onMouseDown={(event) => event.stopPropagation()}
                                onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                              >
                                <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                              </button>
                              <div className="sf-canvas-parameter-heading">
                                <button
                                  type="button"
                                  className="sf-node-ontology-link"
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onClick={(event) => {
                                    event.stopPropagation();
                                    openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                  }}
                                >
                                  {parameter.label || parameter.name}
                                </button>
                              </div>
                              {isJsonParameter(parameter) ? (
                                <div className="sf-json-parameter-actions">
                                  <button
                                    type="button"
                                    className="sf-button secondary"
                                    onClick={() => openJsonEditor(node, parameter)}
                                  >
                                    <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                  </button>
                                </div>
                              ) : (
                                <input
                                  type={
                                    ['integer', 'number', 'real', 'float', 'double'].includes(
                                      String(
                                        Array.isArray(parameter.schema.type)
                                          ? parameter.schema.type[0]
                                          : parameter.schema.type,
                                      ),
                                    )
                                      ? 'number'
                                      : 'text'
                                  }
                                  value={String(value ?? parameter.default ?? parameter.schema.default ?? '')}
                                  onChange={(event) =>
                                    updateNodeParameter(
                                      node.id,
                                      parameter.name,
                                      scalarInputValue(parameter, event.target.value),
                                    )
                                  }
                                />
                              )}
                            </div>
                          );
                        })}
                    </div>
                  ) : null}
                </>
              ) : null}
            </div>
          );
        })}
      </div>
      {tableEditor ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setTableEditor(null)}>
          <section
            className="sf-json-editor-modal sf-table-editor-modal"
            onMouseDown={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <h2>{tableEditor.parameter.label || tableEditor.parameter.name}</h2>
                <small>Type: {schemaTypeLabel(tableEditor.parameter.schema)}</small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close table editor"
                onClick={() => setTableEditor(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            <div className="sf-table-editor-content">
              <table>
                <thead>
                  <tr>
                    {schemaPropertyOrder(tableEditor.parameter.schema).map((columnName) => {
                      const schema = tableEditor.parameter.schema.properties?.[columnName] || {};
                      return (
                        <th key={columnName}>
                          <span>
                            {columnName}
                            {(tableEditor.parameter.schema.required || []).includes(columnName) ? ' *' : ''}
                          </span>
                          <small>{schema.type || 'value'}</small>
                          {schema.type === 'path' ? (
                            <div className="sf-table-editor-path-actions">
                              <button
                                type="button"
                                className="sf-button secondary"
                                onClick={() => void chooseTableColumnPaths(columnName, schema)}
                              >
                                <i className="fa-solid fa-file" /> Choose files
                              </button>
                              <button
                                type="button"
                                className="sf-button secondary"
                                onClick={() => void chooseTableColumnPaths(columnName, schema, true)}
                              >
                                <i className="fa-solid fa-folder" /> Choose folders
                              </button>
                            </div>
                          ) : null}
                        </th>
                      );
                    })}
                    <th aria-label="Row actions" />
                  </tr>
                </thead>
                <tbody>
                  {tableEditor.rows.map((row, rowIndex) => (
                    <tr key={rowIndex}>
                      {schemaPropertyOrder(tableEditor.parameter.schema).map((columnName) => {
                        const schema = tableEditor.parameter.schema.properties?.[columnName] || {};
                        return (
                          <td key={columnName}>
                            <input
                              aria-label={`${schema.title || columnName} row ${rowIndex + 1}`}
                              value={String(row[columnName] ?? '')}
                              onChange={(event) => updateTableCell(rowIndex, columnName, event.target.value)}
                            />
                          </td>
                        );
                      })}
                      <td>
                        <button
                          type="button"
                          className="sf-button secondary"
                          onClick={() =>
                            setTableEditor((current) =>
                              current
                                ? { ...current, rows: current.rows.filter((_, index) => index !== rowIndex) }
                                : current,
                            )
                          }
                        >
                          Remove
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {!tableEditor.rows.length ? <p className="sf-table-editor-empty">No rows added.</p> : null}
            </div>
            {tableEditor.error ? <div className="sf-json-editor-error">{tableEditor.error}</div> : null}
            {csvPreview ? (
              <div className="sf-table-csv-preview">
                <strong>CSV preview</strong>
                <span>{csvPreview.error || `${csvPreview.rows.length} rows ready to import.`}</span>
                {!csvPreview.error ? (
                  <button
                    type="button"
                    className="sf-button secondary"
                    onClick={() => {
                      setTableEditor((current) =>
                        current ? { ...current, rows: [...current.rows, ...csvPreview.rows], error: null } : current,
                      );
                      setCsvPreview(null);
                    }}
                  >
                    Accept preview
                  </button>
                ) : null}
                <button type="button" className="sf-button secondary" onClick={() => setCsvPreview(null)}>
                  Dismiss
                </button>
              </div>
            ) : null}
            <footer>
              <label className="sf-button secondary">
                Import CSV
                <input
                  type="file"
                  accept=".csv,text/csv"
                  hidden
                  onChange={(event) => {
                    const file = event.target.files?.[0];
                    if (file) void importTableCsv(file);
                    event.currentTarget.value = '';
                  }}
                />
              </label>
              <button
                type="button"
                className="sf-button secondary"
                onClick={() =>
                  setTableEditor((current) => {
                    if (!current) return current;
                    const columns = Object.keys(current.parameter.schema.properties || {});
                    return {
                      ...current,
                      rows: [
                        ...current.rows,
                        Object.fromEntries(columns.map((column) => [column, ''])) as Record<string, unknown>,
                      ],
                    };
                  })
                }
              >
                <i className="fa-solid fa-plus" /> Add row
              </button>
              <button type="button" className="sf-button secondary" onClick={() => setTableEditor(null)}>
                Cancel
              </button>
              <button type="button" className="sf-button" onClick={saveTableEditor}>
                Save table
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {jsonEditor ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setJsonEditor(null)}>
          <section className="sf-json-editor-modal" onMouseDown={(event) => event.stopPropagation()}>
            <header>
              <div>
                <h2>{jsonEditor.parameter.label || jsonEditor.parameter.name}</h2>
                <small>Type: {schemaTypeLabel(jsonEditor.parameter.schema)}</small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close JSON editor"
                onClick={() => setJsonEditor(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            <textarea
              value={jsonEditorText}
              spellCheck={false}
              aria-label="JSON value"
              onChange={(event) => {
                setJsonEditorText(event.target.value);
                setJsonEditorError(null);
              }}
            />
            {jsonEditorError ? <div className="sf-json-editor-error">{jsonEditorError}</div> : null}
            <footer>
              <button type="button" className="sf-button secondary" onClick={() => setJsonEditor(null)}>
                Cancel
              </button>
              {isPathListParameter(jsonEditor.parameter) ? (
                <button type="button" className="sf-button secondary" onClick={openPathWizardFromJsonEditor}>
                  <i className="fa-solid fa-folder-tree" /> Add files/folders
                </button>
              ) : null}
              {jsonEditor.parameter.schema.type === 'table' ? (
                <label className="sf-button secondary">
                  <i className="fa-solid fa-file-csv" /> Load CSV
                  <input
                    type="file"
                    accept=".csv,text/csv"
                    hidden
                    onChange={(event) => {
                      const file = event.target.files?.[0];
                      if (file) void importJsonCsv(file);
                      event.currentTarget.value = '';
                    }}
                  />
                </label>
              ) : null}
              {jsonEditor.parameter.schema.type === 'table' ? (
                <button type="button" className="sf-button secondary" onClick={openTableEditorFromJsonEditor}>
                  <i className="fa-solid fa-table" /> Edit as table
                </button>
              ) : null}
              <button type="button" className="sf-button" onClick={saveJsonEditor}>
                Save
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {pathWizard && pathWizardNode ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setPathWizard(null)}>
          <section
            className="sf-json-editor-modal sf-path-wizard-modal"
            onMouseDown={(event) => event.stopPropagation()}
            onWheel={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <h2>{pathWizard.mergeIntoJsonEditor ? 'Add files and folders' : 'Select files and folders'}</h2>
                <small>
                  {pathWizard.mergeIntoJsonEditor
                    ? 'Add files and vendor directories to the JSON string array.'
                    : 'Select multiple files and folders, then connect the output to a compatible array input.'}
                </small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close path selector"
                onClick={() => setPathWizard(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            {client ? (
              <PathFileManager
                client={client}
                selectedPaths={pathWizardEntries}
                onSelectionChange={updatePathWizardSelection}
              />
            ) : null}
            <div className="sf-canvas-file-entries">
              {pathWizardEntries.map((path, index) => (
                <div className="sf-canvas-file-entry" key={`${path}-${index}`}>
                  <input
                    aria-label={`Selected path ${index + 1}`}
                    value={path}
                    onChange={(event) => {
                      const next = [...pathWizardEntries];
                      next[index] = event.target.value;
                      updateNodeParameter(pathWizardNode.id, pathWizard.parameter.name, next);
                    }}
                  />
                  <button
                    type="button"
                    className="sf-button secondary"
                    onClick={() =>
                      updateNodeParameter(
                        pathWizardNode.id,
                        pathWizard.parameter.name,
                        pathWizardEntries.filter((_, itemIndex) => itemIndex !== index),
                      )
                    }
                  >
                    Remove
                  </button>
                </div>
              ))}
              {!pathWizardEntries.length ? <p className="sf-table-editor-empty">No paths selected.</p> : null}
            </div>
            <footer>
              <button type="button" className="sf-button secondary" onClick={() => setPathWizard(null)}>
                Cancel
              </button>
              <button type="button" className="sf-button" onClick={() => setPathWizard(null)}>
                Use selected paths
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {picker ? (
        <div className="sf-canvas-picker sf-operation-deck" onMouseDown={(event) => event.stopPropagation()}>
          <div className="sf-operation-deck-heading">
            <div>
              <strong>{surface === 'explorer' ? 'Choose an operation' : 'Choose the next operation'}</strong>
            </div>
            <button
              type="button"
              className="sf-icon-button"
              aria-label="Close operation palette"
              onClick={() => {
                setPicker(null);
                setPickerCapabilityId(null);
              }}
            >
              <i className="fa-solid fa-xmark" />
            </button>
          </div>
          <div className="sf-operation-deck-search">
            <i className="fa-solid fa-magnifying-glass" aria-hidden="true" />
            <input
              aria-label="Search operation ontology"
              placeholder="Search ontology (regex)"
              value={paletteSearch}
              onChange={(event) => setPaletteSearch(event.target.value)}
            />
            <select
              aria-label="Filter operations by module"
              value={paletteModule}
              onChange={(event) => setPaletteModule(event.target.value)}
            >
              <option value="">All modules</option>
              {paletteModules.map((module) => (
                <option value={module} key={module}>
                  {module}
                </option>
              ))}
            </select>
          </div>
          <div className="sf-operation-deck-list">
            {filteredTemplates.map((template) => {
              const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
              const ports = nodePorts(capability);
              const metadata = `${template.module || 'core'} · ${ports.inputs.length} input${ports.inputs.length === 1 ? '' : 's'} · ${ports.outputs.length} output${ports.outputs.length === 1 ? '' : 's'}`;
              return (
                <article
                  className={`sf-operation-deck-card sf-operation-deck-card-${operationDeckDensity(template.title, metadata)} sf-domain-${(
                    template.domain || 'unknown'
                  )
                    .replace(/[^a-z0-9_-]/gi, '-')
                    .toLowerCase()}`}
                  key={template.id}
                  data-domain={template.domain || 'unknown'}
                >
                  <div className="sf-operation-deck-card-content">
                    <strong>{template.title}</strong>
                    <small>{metadata}</small>
                  </div>
                  <button
                    type="button"
                    className="sf-icon-button"
                    aria-label={`Read about ${template.title}`}
                    onClick={() => setPickerCapabilityId(template.capabilityId || null)}
                  >
                    <i className="fa-solid fa-circle-info" />
                  </button>
                  <button type="button" className="sf-button" onClick={() => addNode(template)}>
                    Add
                  </button>
                </article>
              );
            })}
          </div>
          {!filteredTemplates.length ? (
            <div className="sf-operation-deck-empty">No compatible capability is available for this connection.</div>
          ) : null}
        </div>
      ) : null}
      {documentationCapability ? (
        <div
          className="sf-side-pane-backdrop sf-node-info-backdrop"
          onMouseDown={(event) => {
            if (event.target === event.currentTarget) {
              setDocumentationFocus(null);
              setSelectedNodeId(null);
              setPickerCapabilityId(null);
            }
          }}
        >
          <aside className="sf-side-pane sf-node-info-pane" onMouseDown={(event) => event.stopPropagation()}>
            <div className="sf-node-info-heading">
              <div>
                <h2>{documentationCapability.label}</h2>
              </div>
              <button
                type="button"
                className="sf-icon-button sf-close-button"
                aria-label="Close operation documentation"
                onClick={() => {
                  setDocumentationFocus(null);
                  setSelectedNodeId(null);
                  setPickerCapabilityId(null);
                }}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </div>
            <p>{documentationCapability.definition}</p>
            {documentationCapability.interface.guidance ? (
              <section className="sf-node-info-guidance">
                <h3>Guidance</h3>
                <p>{documentationCapability.interface.guidance}</p>
              </section>
            ) : null}
            <section>
              <h3>Inputs</h3>
              {nodePorts(documentationCapability).inputs.map((port) => (
                <div
                  className="sf-info-row"
                  key={port.id}
                  data-ontology-focus="true"
                  data-ontology-focus-section="inputs"
                  data-ontology-focus-key={port.id}
                  tabIndex={-1}
                >
                  {ontologyPortTerm(port) && onOpenOntologyWiki ? (
                    <button
                      type="button"
                      className="sf-ontology-term-link"
                      onClick={() => onOpenOntologyWiki(ontologyPortTerm(port))}
                    >
                      {port.label}
                    </button>
                  ) : (
                    <strong>{port.label}</strong>
                  )}
                  <span>
                    {port.required ? 'required' : 'optional'}
                    {port.description ? (
                      <>
                        <br />
                        <small>{port.description}</small>
                      </>
                    ) : null}
                    {portOntologyDetails(port).map((detail) => (
                      <Fragment key={detail}>
                        <br />
                        <small>{detail}</small>
                      </Fragment>
                    ))}
                  </span>
                </div>
              ))}
            </section>
            <section>
              <h3>Outputs</h3>
              {nodePorts(documentationCapability).outputs.map((port) => (
                <div
                  className="sf-info-row"
                  key={port.id}
                  data-ontology-focus="true"
                  data-ontology-focus-section="outputs"
                  data-ontology-focus-key={port.id}
                  tabIndex={-1}
                >
                  {ontologyPortTerm(port) && onOpenOntologyWiki ? (
                    <button
                      type="button"
                      className="sf-ontology-term-link"
                      onClick={() => onOpenOntologyWiki(ontologyPortTerm(port))}
                    >
                      {port.label}
                    </button>
                  ) : (
                    <strong>{port.label}</strong>
                  )}
                  <span>
                    {port.description || 'value'}
                    {portOntologyDetails(port).map((detail) => (
                      <Fragment key={detail}>
                        <br />
                        <small>{detail}</small>
                      </Fragment>
                    ))}
                  </span>
                </div>
              ))}
            </section>
            <section>
              <h3>Parameters</h3>
              {documentationCapability.parameters
                .filter((parameter) => parameter.name !== 'database_path')
                .map((parameter) => (
                  <div
                    className="sf-info-row"
                    key={parameter.name}
                    data-ontology-focus="true"
                    data-ontology-focus-section="parameters"
                    data-ontology-focus-key={parameter.name}
                    tabIndex={-1}
                  >
                    {onOpenOntologyWiki ? (
                      <button
                        type="button"
                        className="sf-ontology-term-link"
                        onClick={() => onOpenOntologyWiki(parameter.name)}
                      >
                        {parameter.label || parameter.name}
                      </button>
                    ) : (
                      <strong>{parameter.label || parameter.name}</strong>
                    )}
                    <span>
                      {parameter.description || parameter.schema.type || 'value'}
                      <br />
                      <small>type: {schemaTypeLabel(parameter.schema)}</small>
                      {parameterOntologyDetails(parameter).map((detail) => (
                        <Fragment key={detail}>
                          <br />
                          <small>{detail}</small>
                        </Fragment>
                      ))}
                      {parameter.default !== undefined || parameter.schema.default !== undefined ? (
                        <>
                          <br />
                          <small>Default: {JSON.stringify(parameter.default ?? parameter.schema.default)}</small>
                        </>
                      ) : null}
                      {Array.isArray(parameter.schema.examples) && parameter.schema.examples.length ? (
                        <>
                          <br />
                          <small>
                            Examples:{' '}
                            {JSON.stringify(
                              parameter.schema.examples.length === 1
                                ? parameter.schema.examples[0]
                                : parameter.schema.examples,
                            )}
                          </small>
                        </>
                      ) : null}
                    </span>
                  </div>
                ))}
            </section>
          </aside>
        </div>
      ) : null}
      {artifactViewer ? (
        <div className="sf-artifact-viewer-backdrop" role="presentation" onMouseDown={() => setArtifactViewer(null)}>
          <section
            className={`sf-artifact-viewer ${
              isVisualizationArtifact(artifactViewer)
                ? 'visualization'
                : artifactViewer.representation === 'table'
                  ? 'wide'
                  : 'json'
            }`}
            role="dialog"
            aria-modal="true"
            aria-label="Artifact viewer"
            onMouseDown={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <h2>{artifactViewer.contract_id}</h2>
                <small>
                  {artifactViewer.representation} · {artifactViewer.artifact_id}
                </small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close artifact viewer"
                onClick={() => setArtifactViewer(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            {artifactViewer.representation === 'table' ? (
              <>
                <div className="sf-artifact-viewer-toolbar">
                  <input
                    value={artifactViewerSearch}
                    onChange={(event) => setArtifactViewerSearch(event.target.value)}
                    placeholder="Search all columns"
                    aria-label="Search all columns"
                  />
                  <select
                    value={artifactViewerSort}
                    onChange={(event) => setArtifactViewerSort(event.target.value)}
                    aria-label="Sort by column"
                  >
                    <option value="">Natural order</option>
                    {(artifactData?.columns ?? artifactViewer.columns ?? []).map((column) => (
                      <option key={column.name} value={column.name}>
                        {column.name}
                      </option>
                    ))}
                  </select>
                  <button
                    type="button"
                    onClick={() => setArtifactViewerDescending((value) => !value)}
                    disabled={!artifactViewerSort}
                  >
                    {artifactViewerDescending ? 'Descending' : 'Ascending'}
                  </button>
                </div>
                <VirtualArtifactTable
                  columns={artifactData?.columns ?? artifactViewer.columns ?? []}
                  pages={artifactPages}
                  pageOffset={artifactData?.offset ?? 0}
                  loading={artifactViewerLoading}
                />
                <footer>
                  <span>
                    {artifactData
                      ? `Showing rows ${artifactData.offset + 1}-${Math.min(artifactData.offset + artifactData.rows.length, artifactData.total_rows)} of ${artifactData.total_rows}`
                      : 'No rows loaded'}
                    . Search and sorting are performed by the service.
                  </span>
                  <button
                    type="button"
                    onClick={() => requestArtifactPage(Math.max(0, (artifactData?.offset ?? 0) - ARTIFACT_PAGE_SIZE))}
                    disabled={!artifactData || artifactData.offset === 0 || artifactViewerLoading}
                  >
                    Previous page
                  </button>
                  <button
                    type="button"
                    onClick={() => requestArtifactPage((artifactData?.offset ?? 0) + ARTIFACT_PAGE_SIZE)}
                    disabled={!artifactData || artifactData.offset + artifactData.rows.length >= artifactData.total_rows || artifactViewerLoading}
                  >
                    Next page
                  </button>
                </footer>
              </>
            ) : isVisualizationArtifact(artifactViewer) ? (
              <VisualizationArtifactPreview artifact={artifactViewer} />
            ) : (
              <pre className="sf-artifact-viewer-json">{prettyArtifactPayload(artifactViewer.payload)}</pre>
            )}
          </section>
        </div>
      ) : null}
    </div>
  );
}
