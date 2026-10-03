import type { BackendCapability, CapabilityParameter, JsonSchema, JsonValue } from '../../framework/backend/protocol';

export function defaultParameters(capability: BackendCapability): Record<string, unknown> {
  return Object.fromEntries(
    capability.parameters.map((parameter) => [
      parameter.name,
      parameter.default ??
        parameter.schema.default ??
        (parameter.schema.type === 'array' || parameter.schema.type === 'table' ? [] : ''),
    ]),
  );
}

export function schemaTypeLabel(schema: JsonSchema | undefined): string {
  if (!schema) return 'unknown';
  const type = Array.isArray(schema.type) ? schema.type.join(' | ') : schema.type || 'value';
  if (type === 'array' && schema.items) return `array<${schemaTypeLabel(schema.items)}>`;
  return type;
}

export function fileParameterExtensions(parameter: CapabilityParameter): string[] {
  if (parameter.extensions?.length) return parameter.extensions;
  const itemExtensions = parameter.schema.items?.extensions;
  return Array.isArray(itemExtensions) ? itemExtensions.filter((item): item is string => typeof item === 'string') : [];
}

export function isSimplePathParameter(parameter: CapabilityParameter): boolean {
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

export function isFileListParameter(parameter: CapabilityParameter): boolean {
  if (parameter.schema.type !== 'array' || parameter.schema.items?.type !== 'object') return false;
  if (parameter.extensions?.length || parameter.path_kind) return true;
  const properties = parameter.schema.items.properties;
  return Boolean(
    properties &&
    Object.values(properties).some(
      (property) => property.type === 'path' || property.format === 'path' || property.format === 'file-path',
    ),
  );
}

export function isPathListParameter(parameter: CapabilityParameter): boolean {
  return isSimplePathParameter(parameter) || isFileListParameter(parameter);
}

export function isTableParameter(parameter: CapabilityParameter): boolean {
  return parameter.schema.type === 'table' && Boolean(parameter.schema.properties);
}

export function isJsonParameter(parameter: CapabilityParameter): boolean {
  return (
    parameter.schema.type === 'table' ||
    parameter.schema.type === 'object' ||
    (parameter.schema.type === 'array' && !isFileListParameter(parameter))
  );
}

export function scalarInputValue(parameter: CapabilityParameter, raw: string): unknown {
  const type = Array.isArray(parameter.schema.type) ? parameter.schema.type[0] : parameter.schema.type;
  if (type === 'integer') return raw === '' ? '' : Number.parseInt(raw, 10);
  if (type === 'number' || type === 'real' || type === 'float' || type === 'double')
    return raw === '' ? '' : Number.parseFloat(raw);
  if (type === 'boolean') return raw === 'true';
  return raw;
}

export function wireTableToRows(value: unknown): unknown {
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

export function uiParameters(
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

export function wireParameters(
  capability: BackendCapability | undefined,
  parameters: Record<string, unknown>,
): Record<string, JsonValue> {
  if (!capability) return JSON.parse(JSON.stringify(parameters)) as Record<string, JsonValue>;
  return Object.fromEntries(
    Object.entries(parameters).filter(([name]) => capability.parameters.some((candidate) => candidate.name === name)),
  ) as Record<string, JsonValue>;
}

export function schemaPropertyOrder(schema: JsonSchema): string[] {
  const properties = schema.properties || {};
  const declared = schema['x-streamfind-property-order'];
  const ordered = Array.isArray(declared) ? declared.filter((item): item is string => typeof item === 'string') : [];
  return [
    ...ordered.filter((name) => name in properties),
    ...Object.keys(properties).filter((name) => !ordered.includes(name)),
  ];
}

export function validateInlineTableRows(
  parameter: CapabilityParameter,
  rows: Record<string, unknown>[],
): string | null {
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

export function normalizeInlineTableRows(parameter: CapabilityParameter, rows: Record<string, unknown>[]) {
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

export function parseCsvRows(text: string, columns: string[]): { rows?: Record<string, unknown>[]; error?: string } {
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

export function validateJsonValue(value: unknown, schema: JsonSchema, path = 'value'): string | null {
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
        } else if (schema.additionalProperties === false) return `${path} contains unknown property “${name}”.`;
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
