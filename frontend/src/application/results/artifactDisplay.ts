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
  const contract = localArtifactContract(artifact.contract_id);
  const statisticsSummary = summarizeStatisticsResult(contract, artifact.payload);
  if (statisticsSummary) return `JSON\n${statisticsSummary}`;
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

function localArtifactContract(value: string): string {
  return value.split('#').at(-1)?.split(':').at(-1) ?? value;
}

function parseObject(payload: ArtifactRecord['payload']): Record<string, unknown> | null {
  let value: unknown = payload;
  if (typeof value === 'string') {
    try {
      value = JSON.parse(value);
    } catch {
      return null;
    }
  }
  return value && typeof value === 'object' && !Array.isArray(value) ? (value as Record<string, unknown>) : null;
}

function summaryValue(payload: Record<string, unknown>, key: string): string | null {
  const value = payload[key];
  if (value === undefined || value === null || value === '') return null;
  return typeof value === 'number' ? value.toLocaleString('en-US') : String(value);
}

function summarizeStatisticsResult(contract: string, payload: ArtifactRecord['payload']): string | null {
  const values = parseObject(payload);
  if (!values) return null;
  if (contract === 'foldChangeDiagnostics') {
    const parts = [
      summaryValue(values, 'samples_group_a') && `${summaryValue(values, 'samples_group_a')} group A samples`,
      summaryValue(values, 'samples_group_b') && `${summaryValue(values, 'samples_group_b')} group B samples`,
      summaryValue(values, 'variables_tested') && `${summaryValue(values, 'variables_tested')} variables tested`,
      summaryValue(values, 'significant_variables') && `${summaryValue(values, 'significant_variables')} significant`,
    ].filter((part): part is string => Boolean(part));
    return parts.length ? parts.join(' · ') : 'Fold-change analysis summary';
  }
  if (contract === 'pcaDiagnostics') {
    const parts = [
      summaryValue(values, 'samples') && `${summaryValue(values, 'samples')} samples`,
      summaryValue(values, 'variables') && `${summaryValue(values, 'variables')} variables`,
      summaryValue(values, 'components') && `${summaryValue(values, 'components')} components`,
    ].filter((part): part is string => Boolean(part));
    return parts.length ? `PCA diagnostics · ${parts.join(' · ')}` : 'PCA diagnostics';
  }
  if (contract === 'plsDiagnostics') {
    const parts = [
      summaryValue(values, 'samples') && `${summaryValue(values, 'samples')} samples`,
      summaryValue(values, 'predictors') && `${summaryValue(values, 'predictors')} predictors`,
      summaryValue(values, 'responses') && `${summaryValue(values, 'responses')} responses`,
      summaryValue(values, 'components') && `${summaryValue(values, 'components')} components`,
    ].filter((part): part is string => Boolean(part));
    return parts.length ? `PLS diagnostics · ${parts.join(' · ')}` : 'PLS diagnostics';
  }
  return null;
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
