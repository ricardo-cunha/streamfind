import { describe, expect, it } from 'vitest';
import { visualizationSpecFromArtifact } from './VisualizationDataResolver';

const payload = {
  schema: 'streamfind.visualization/v1' as const,
  visualization_id: 'mass_spec.plot_spectra_tic',
  semantic_type: 'mass_spec.spectra_tic',
  title: 'TIC',
  data_mode: 'inline' as const,
  renderer: { engine: 'plotly' as const, renderer_id: 'core.plotly' as const, spec_version: '1' as const },
  payload: { data: [{ type: 'scatter', x: [1], y: [2] }] },
  provenance: { source_artifact_ids: ['spectra-1'], producer_operation_id: 'plot', producer_node_id: 'node-1' },
  fallback: { description: 'TIC unavailable.' },
};

describe('VisualizationDataResolver', () => {
  it('resolves only JSON visualization result artifacts', () => {
    expect(
      visualizationSpecFromArtifact({
        artifact_id: 'vis-1',
        contract_id: 'visualizationSpecResult',
        representation: 'json',
        payload,
        producer_operation: 'plot',
        producer_instance: 'node-1',
        workflow_revision: 1,
        status: 'published',
      }),
    ).toEqual(payload);
  });

  it('rejects non-visualization artifacts', () => {
    expect(() =>
      visualizationSpecFromArtifact({
        artifact_id: 'table-1',
        contract_id: 'ticTable',
        representation: 'table',
        producer_operation: 'get_spectra_tic',
        producer_instance: 'node-1',
        workflow_revision: 1,
        status: 'published',
      }),
    ).toThrow(/not a visualization/);
  });
});
