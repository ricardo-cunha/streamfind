import type { ResultRecord } from '../../framework/backend/StreamFindApiClient';
import type { JsonValue } from '../../framework/backend/protocol';

export function artifactSummary(artifact: ResultRecord): string {
  if (artifact.representation === 'table') {
    const columns = Array.isArray(artifact.columns) ? artifact.columns : [];
    const names = columns
      .slice(0, 6)
      .map((column) => column.name)
      .join(', ');
    return `${artifact.physical_table || 'Result table'}\n${columns.length} columns · ${artifact.row_count ?? 0} rows\n${names}${columns.length > 6 ? ', …' : ''}`;
  }
  if (isVisualizationArtifact(artifact)) {
    let visualization: unknown = artifact.payload;
    if (typeof visualization === 'string') {
      try {
        visualization = JSON.parse(visualization);
      } catch {
        return 'JSON\nVisualization specification';
      }
    }
    const traces =
      visualization && typeof visualization === 'object' && !Array.isArray(visualization)
        ? (visualization as { payload?: { data?: unknown } }).payload?.data
        : undefined;
    const lengths = Array.isArray(traces)
      ? traces.map((trace) => {
          if (!trace || typeof trace !== 'object' || Array.isArray(trace)) return 0;
          const candidate = trace as { x?: unknown; y?: unknown };
          return Array.isArray(candidate.x) ? candidate.x.length : Array.isArray(candidate.y) ? candidate.y.length : 0;
        })
      : [];
    const totalPoints = lengths.reduce((total, length) => total + length, 0);
    const minimum = lengths.length ? Math.min(...lengths) : 0;
    const maximum = lengths.length ? Math.max(...lengths) : 0;
    const traceLength = minimum === maximum ? `${minimum}` : `${minimum}–${maximum}`;
    return `JSON\n${lengths.length} legend traces · ${totalPoints} total points · trace length ${traceLength}`;
  }
  return `JSON\n${summarizeJsonPayload(artifact.payload)}`;
}

function parseJsonPayload(payload: ResultRecord['payload']): JsonValue | null {
  let value: unknown = payload;
  if (typeof value === 'string') {
    try {
      value = JSON.parse(value);
    } catch {
      return null;
    }
  }
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

export function isVisualizationArtifact(artifact: ResultRecord): boolean {
  const contract = artifact.contract_id.split('#').at(-1)?.split(':').at(-1) ?? artifact.contract_id;
  return contract === 'visualizationSpecResult';
}

export function isJsonArtifact(artifact: ResultRecord): boolean {
  return artifact.representation === 'json';
}
