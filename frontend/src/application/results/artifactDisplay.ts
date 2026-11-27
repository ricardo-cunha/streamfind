import type { ResultRecord } from '../../framework/backend/StreamFindApiClient';
import type { JsonValue } from '../../framework/backend/protocol';

export function artifactSummary(artifact: ResultRecord): string {
  if (artifact.representation === 'table') {
    const columns = Array.isArray(artifact.columns) ? artifact.columns : [];
    const names = columns
      .slice(0, 6)
      .map((column) => column.name)
      .join(', ');
    return `Arrow table result\n${columns.length} columns · ${artifact.row_count ?? 0} rows\n${names}${columns.length > 6 ? ', …' : ''}`;
  }
  if (artifact.payload === undefined) return 'JSON result · open to view';
  return `JSON\n${summarizeJsonPayload(artifact.payload)}`;
}

function parseJsonPayload(payload: ResultRecord['payload']): JsonValue | null {
  const value: unknown = payload;
  if (
    value === null ||
    typeof value === 'string' ||
    typeof value === 'number' ||
    typeof value === 'boolean' ||
    Array.isArray(value) ||
    (typeof value === 'object' && value !== null)
  ) {
    return value as JsonValue;
  }
  return null;
}

function summarizeJsonPayload(payload: ResultRecord['payload']): string {
  const value = parseJsonPayload(payload);
  if (value === null) return 'Text value';
  if (Array.isArray(value)) return `Array · ${value.length.toLocaleString('en-US')} items`;
  if (typeof value === 'object') return `Object · ${Object.keys(value).length.toLocaleString('en-US')} fields`;
  if (typeof value === 'string') return `String · ${value.length.toLocaleString('en-US')} characters`;
  return `${typeof value} value`;
}

export function prettyArtifactPayload(payload: ResultRecord['payload']): string {
  return JSON.stringify(payload ?? null, null, 2);
}

export function artifactContractMatches(artifactContract: string, portContract?: string): boolean {
  if (!portContract) return false;
  if (artifactContract === portContract) return true;
  const local = (value: string) => value.split('#').at(-1)?.split(':').at(-1) ?? value;
  return local(artifactContract) === local(portContract);
}

export function isJsonArtifact(artifact: ResultRecord): boolean {
  return artifact.representation === 'json';
}
