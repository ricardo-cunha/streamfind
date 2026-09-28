import { render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { PlotlyRenderer, type PlotlyRuntime } from './PlotlyRenderer';
import type { VisualizationSpec } from './visualizationTypes';

const spec: VisualizationSpec = {
  schema: 'streamfind.visualization/v1',
  visualization_id: 'mass_spec.plot_spectra_tic',
  semantic_type: 'mass_spec.spectra_tic',
  title: 'TIC',
  data_mode: 'inline',
  renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
  payload: { data: [{ type: 'scatter', x: [1], y: [2] }] },
  provenance: { source_artifact_ids: ['spectra-1'], producer_operation_id: 'plot', producer_node_id: 'node-1' },
  fallback: { description: 'TIC unavailable.' },
};

describe('PlotlyRenderer', () => {
  afterEach(() => vi.restoreAllMocks());

  it('passes validated backend data to Plotly and purges on unmount', async () => {
    const runtime: PlotlyRuntime = {
      newPlot: vi.fn(),
      purge: vi.fn(),
    };
    const view = render(<PlotlyRenderer runtime={runtime} spec={spec} />);
    await vi.waitFor(() => expect(runtime.newPlot).toHaveBeenCalledOnce());
    expect(runtime.newPlot).toHaveBeenCalledWith(
      expect.any(HTMLElement),
      spec.payload.data,
      { ...spec.payload.layout, autosize: true },
      spec.payload.config,
    );
    view.rerender(<PlotlyRenderer runtime={runtime} spec={{ ...spec, title: 'TIC (updated label)' }} />);
    expect(runtime.newPlot).toHaveBeenCalledOnce();
    expect(screen.queryByText('TIC unavailable.')).not.toBeInTheDocument();
    view.unmount();
    expect(runtime.purge).toHaveBeenCalledOnce();
  });
});
