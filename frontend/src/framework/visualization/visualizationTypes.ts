import type { JsonValue } from '../backend/protocol';

export const VISUALIZATION_SPEC_VERSION = 'streamfind.visualization/v1' as const;

export type VisualizationRendererId = 'core.plotly' | 'core.d3';
export type VisualizationDataMode = 'inline' | 'artifact';
export type PlotlyValue = JsonValue | undefined;

export type PlotlyTrace = {
  type: string;
  name?: string;
  [key: string]: PlotlyValue;
};

export type PlotlyPayload = {
  data: PlotlyTrace[];
  layout?: Record<string, JsonValue>;
  config?: Record<string, JsonValue>;
};

export type VisualizationSpec = {
  schema: typeof VISUALIZATION_SPEC_VERSION;
  visualization_id: string;
  semantic_type: string;
  title: string;
  data_mode: VisualizationDataMode;
  renderer: {
    engine: 'plotly';
    renderer_id: 'core.plotly';
    spec_version: '1';
  };
  payload: PlotlyPayload;
  provenance: {
    source_artifact_ids: string[];
    producer_operation_id: string;
    producer_node_id: string;
  };
  fallback: { description: string };
};

export function isVisualizationSpec(value: unknown): value is VisualizationSpec {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const candidate = value as Partial<VisualizationSpec>;
  if (candidate.schema !== VISUALIZATION_SPEC_VERSION) return false;
  if (typeof candidate.visualization_id !== 'string' || typeof candidate.semantic_type !== 'string') return false;
  if (typeof candidate.title !== 'string') return false;
  if (candidate.data_mode !== 'inline') return false;
  const renderer = candidate.renderer;
  if (
    !renderer ||
    renderer.engine !== 'plotly' ||
    renderer.renderer_id !== 'core.plotly' ||
    renderer.spec_version !== '1'
  ) {
    return false;
  }
  const payload = candidate.payload;
  if (!payload || !Array.isArray(payload.data) || payload.data.length === 0) return false;
  if (!payload.data.every((trace) => trace && typeof trace === 'object' && typeof trace.type === 'string'))
    return false;
  const provenance = candidate.provenance;
  if (
    !provenance ||
    !Array.isArray(provenance.source_artifact_ids) ||
    provenance.source_artifact_ids.length === 0 ||
    !provenance.source_artifact_ids.every((id) => typeof id === 'string') ||
    typeof provenance.producer_operation_id !== 'string' ||
    typeof provenance.producer_node_id !== 'string'
  ) {
    return false;
  }
  return Boolean(candidate.fallback && typeof candidate.fallback.description === 'string');
}

export function parseVisualizationSpec(payload: unknown): VisualizationSpec {
  const parsed = typeof payload === 'string' ? JSON.parse(payload) : payload;
  if (!isVisualizationSpec(parsed)) {
    throw new Error('Artifact payload is not a valid StreamFind visualization specification');
  }
  return parsed;
}
