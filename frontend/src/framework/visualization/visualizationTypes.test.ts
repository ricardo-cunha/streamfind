import { describe, expect, it } from 'vitest';
import { VisualizationRegistry } from './VisualizationRegistry';
import { isVisualizationSpec, parseVisualizationSpec, VISUALIZATION_SPEC_VERSION } from './visualizationTypes';

const spec = {
  schema: VISUALIZATION_SPEC_VERSION,
  visualization_id: 'mass_spec.plot_spectra_tic',
  semantic_type: 'mass_spec.spectra_tic',
  data_mode: 'inline' as const,
  title: 'TIC',
  renderer: { engine: 'plotly' as const, renderer_id: 'core.plotly' as const, spec_version: '1' as const },
  payload: { data: [{ type: 'scatter', x: [1, 2], y: [3, 4] }] },
  provenance: {
    source_artifact_ids: ['spectra-1'],
    producer_operation_id: 'mass_spec.plot_spectra_tic',
    producer_node_id: 'tic-1',
  },
  fallback: { description: 'TIC visualization unavailable.' },
};

describe('visualization contracts', () => {
  it('accepts the bounded v1 envelope and parses JSON payloads', () => {
    expect(isVisualizationSpec(spec)).toBe(true);
    expect(parseVisualizationSpec(JSON.stringify(spec))).toEqual(spec);
  });

  it('rejects an empty trace payload', () => {
    expect(isVisualizationSpec({ ...spec, payload: { data: [] } })).toBe(false);
  });

  it('registers and resolves renderers deterministically', () => {
    const registry = new VisualizationRegistry();
    const renderer = () => null;
    registry.register('core.plotly', renderer);
    expect(registry.resolve('core.plotly')).toBe(renderer);
    expect(registry.ids()).toEqual(['core.plotly']);
    expect(() => registry.register('core.plotly', renderer)).toThrow(/already registered/);
  });
});
