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

  it('clears selection when Plotly reports a click without a point', async () => {
    let clickHandler: ((event: { points?: Array<{ pointIndex?: number; pointNumber?: number }> }) => void) | undefined;
    let doubleClickHandler: (() => void) | undefined;
    type PlotElement = HTMLDivElement & {
      on?: (
        event: string,
        handler: (event: { points?: Array<{ pointIndex?: number; pointNumber?: number }> }) => void,
      ) => void;
    };
    const runtime: PlotlyRuntime = {
      newPlot: vi.fn((element) => {
        (element as PlotElement).on = (_event, handler) => {
          if (_event === 'plotly_click') clickHandler = handler;
          if (_event === 'plotly_doubleclick') doubleClickHandler = () => handler({ points: [] });
        };
      }),
    };
    const onPointClick = vi.fn();
    const onPlotClick = vi.fn();
    const onDoubleClick = vi.fn();
    render(
      <PlotlyRenderer
        runtime={runtime}
        spec={spec}
        onPointClick={onPointClick}
        onPlotClick={onPlotClick}
        onDoubleClick={onDoubleClick}
      />,
    );
    await vi.waitFor(() => expect(clickHandler).toBeDefined());

    clickHandler?.({ points: [] });

    expect(onPointClick).not.toHaveBeenCalled();
    expect(onPlotClick).toHaveBeenCalledOnce();
    doubleClickHandler?.();
    expect(onDoubleClick).toHaveBeenCalledOnce();
  });
});
