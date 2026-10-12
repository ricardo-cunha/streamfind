import type { ArtifactRecord } from '../../framework/backend/StreamFindApiClient';
import type { JsonValue } from '../../framework/backend/protocol';

export function artifactSummary(artifact: ArtifactRecord): string {
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

export function prettyArtifactPayload(payload: ArtifactRecord['payload']): string {
  if (typeof payload !== 'string') return JSON.stringify(payload ?? null, null, 2);
  try {
    return JSON.stringify(JSON.parse(payload) as JsonValue, null, 2);
  } catch {
    return payload;
  }
}

export function artifactContractMatches(artifactContract: string, portContract?: string): boolean {
  if (!portContract) return false;
  if (artifactContract === portContract) return true;
  const local = (value: string) => value.split('#').at(-1)?.split(':').at(-1) ?? value;
  return local(artifactContract) === local(portContract);
}

export function isVisualizationArtifact(artifact: ArtifactRecord): boolean {
  const contract = artifact.contract_id.split('#').at(-1)?.split(':').at(-1) ?? artifact.contract_id;
  return contract === 'visualizationSpecResult';
}
