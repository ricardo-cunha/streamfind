import type { CapabilityParameter, JsonSchema } from '../../framework/backend/protocol';
import { schemaTypeLabel } from './parameterModel';
import type { NodePort } from './portModel';

export function schemaOntologyDetails(schema: JsonSchema | undefined): string[] {
  if (!schema) return [];
  const details: string[] = [];
  const pathKind = schema.path_kind;
  const extensions = schema.extensions;
  if (typeof pathKind === 'string') details.push(`path kind: ${pathKind}`);
  if (Array.isArray(extensions) && extensions.every((extension) => typeof extension === 'string'))
    details.push(`extensions: ${extensions.join(', ')}`);
  return details;
}

export function portOntologyDetails(port: NodePort): string[] {
  return [`type: ${port.schema ? schemaTypeLabel(port.schema) : port.dataKind || 'value'}`];
}

export function parameterOntologyDetails(parameter: CapabilityParameter): string[] {
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

export function ontologyPortTerm(port: NodePort): string {
  return port.table?.table_contract_name || port.semanticContract || port.id;
}
