import type {
  BackendCapability,
  CapabilityParameter,
  CapabilityPort,
  JsonSchema,
} from '../../framework/backend/protocol';
import { schemaTypeLabel } from './parameterModel';

export type NodePort = {
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

export function portTypeKey(port: CapabilityPort | NodePort): string {
  const dataKind = 'dataKind' in port ? port.dataKind : 'data_kind' in port ? port.data_kind : undefined;
  const contract =
    'semanticContract' in port
      ? port.semanticContract
      : 'semantic_contract' in port
        ? port.semantic_contract
        : undefined;
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

export function visualPortTypeKey(port: NodePort): string {
  const schemaType = Array.isArray(port.schema?.type) ? port.schema.type[0] : port.schema?.type;
  if (schemaType === 'array') return schemaTypeLabel(port.schema);
  if (schemaType === 'object') return 'object';
  if (schemaType === 'table') return 'table';
  return port.typeKey;
}

export function parameterTypeKey(parameter: CapabilityParameter): string {
  const type = Array.isArray(parameter.schema.type) ? parameter.schema.type.join('|') : parameter.schema.type;
  if (type === 'table') return 'table';
  if (type === 'array') return schemaTypeLabel(parameter.schema);
  if (type === 'object') return 'object';
  return type || 'unknown';
}

export function schemaShape(schema: JsonSchema | undefined): string {
  if (!schema) return '';
  const normalized: Record<string, unknown> = {};
  const schemaType = schema.type ?? (schema.properties ? 'table' : undefined);
  for (const key of ['type', 'properties', 'items', 'required', 'additionalProperties', 'enum']) {
    if (key === 'type' && schemaType !== undefined) normalized.type = schemaType;
    if (key === 'additionalProperties' && schemaType === 'table') continue;
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
  if (normalized.items && typeof normalized.items === 'object')
    normalized.items = schemaShape(normalized.items as JsonSchema);
  return JSON.stringify(normalized);
}

export function typeIcon(typeKey: string): string {
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

export function typeClass(typeKey: string): string {
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

export function nodePorts(capability: BackendCapability | undefined): { inputs: NodePort[]; outputs: NodePort[] } {
  const canvas = capability?.canvas;
  const mergePorts = (...groups: Array<CapabilityPort[] | undefined>): CapabilityPort[] => {
    const ports = new Map<string, CapabilityPort>();
    groups
      .flatMap((group) => group || [])
      .forEach((port) => {
        if (!ports.has(port.id)) ports.set(port.id, port);
        else ports.set(port.id, { ...ports.get(port.id), ...port });
      });
    return [...ports.values()];
  };
  const inputPorts = mergePorts(canvas?.input_ports, capability?.input_ports, capability?.inputs);
  const outputPorts = mergePorts(
    (canvas as { output_ports?: CapabilityPort[] } | undefined)?.output_ports,
    capability?.output_ports,
    capability?.outputs,
  );
  const mapPort = (port: CapabilityPort, required?: boolean): NodePort => ({
    id: port.id,
    label: port.label || port.id,
    required,
    description: port.schema?.description,
    table: port.table,
    semanticContract: port.semantic_contract,
    dataKind: port.data_kind,
    schema: port.schema,
    representations: port.representations,
    typeKey: portTypeKey(port),
  });
  return {
    inputs: inputPorts.map((port: CapabilityPort) => mapPort(port, port.required ?? port.optional !== true)),
    outputs: outputPorts.map((port: CapabilityPort) => mapPort(port)),
  };
}

export function portMatchesParameter(sourcePort: NodePort, parameter: CapabilityParameter): boolean {
  const parameterKind =
    parameter.schema.type === 'table'
      ? 'table'
      : parameter.schema.type === 'object' || parameter.schema.type === 'array'
        ? 'structured_value'
        : parameter.schema.type;
  const sourceKind = sourcePort.dataKind === 'duckdb_table' ? 'table' : sourcePort.dataKind;
  return schemaShape(sourcePort.schema) === schemaShape(parameter.schema) && sourceKind === parameterKind;
}
