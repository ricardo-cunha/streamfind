import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ResultRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../../framework/plugins/pluginTypes';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerContext } from '../../framework/viewers/viewerTypes';
import { ChromatogramsTableViewer } from './ChromatogramsTableViewer';

vi.mock('../../framework/visualization/VisualizationRenderer', () => ({
  VisualizationRenderer: ({ spec }: { spec: VisualizationSpec }) => (
    <output data-testid="chromatogram-plot">{JSON.stringify(spec)}</output>
  ),
}));

const artifact: ResultRecord = {
  result_id: 'chromatograms-1',
  contract_id: 'chromatogramsTable',
  representation: 'table',
  producer_operation: 'mass_spec.load_chromatograms',
  producer_instance: 'node-2',
  workflow_revision: 1,
  status: 'published',
};

const row = {
  analysis: 'sample-1',
  index: '4',
  chromatogram_id: 'SRM SIC Q1=75 Q3=58 name=NDMA_58',
  polarity: '1',
  precursor_mz: '75',
  product_mz: '58',
  signal_type: 'MS',
  chromatogram_type: 'SRM',
  detector: 'SCIEX',
  rt: '128',
  raw_intensity: '100',
  baseline: '2',
  intensity: '98',
};

afterEach(() => cleanup());

describe('ChromatogramsTableViewer', () => {
  it('queries raw chromatogram points and renders a metadata-aware trace', async () => {
    const resultQuery = vi.fn().mockImplementation((_sessionId, request) =>
      Promise.resolve(
        request.mode === 'distinct'
          ? {
              result_id: 'chromatograms-1',
              columns: [],
              rows: [],
              offset: 0,
              limit: 0,
              total_rows: 1,
              returned_rows: 0,
              mode: 'distinct',
              has_more: false,
              distinct: { precursor_mz: [75, 81] },
            }
          : {
              result_id: 'chromatograms-1',
              columns: [],
              rows: [row],
              offset: 0,
              limit: 1,
              total_rows: 1,
              returned_rows: 1,
              mode: request.mode,
              has_more: false,
            },
      ),
    );
    const client = { resultQuery } as unknown as StreamFindApiClient;
    const context: ViewerContext = {
      sessionId: 'session-1',
      resultId: 'chromatograms-1',
      semanticType: 'chromatogramsTable',
      artifact,
      pluginApi: { client } as FrontendPluginApi,
    };

    render(<ChromatogramsTableViewer context={context} />);

    await waitFor(() => expect(screen.getByTestId('chromatogram-plot')).toBeInTheDocument());
    const request = resultQuery.mock.calls.find(([, value]) => value.mode === 'sample')?.[1];
    expect(request.mode).toBe('sample');
    expect(request.columns).toEqual(
      expect.arrayContaining(['chromatogram_id', 'precursor_mz', 'product_mz', 'intensity']),
    );
    expect(request.columns).not.toContain('raw_intensity');
    expect(request.columns).not.toContain('baseline');
    expect(screen.getByTestId('chromatogram-plot').textContent).toMatch(/Q1 75 → Q3 58/);
  });

  it('supports multiple numeric metadata values in one filter', async () => {
    const resultQuery = vi.fn().mockImplementation((_sessionId, request) =>
      Promise.resolve(
        request.mode === 'distinct'
          ? {
              result_id: 'chromatograms-1',
              columns: [],
              rows: [],
              offset: 0,
              limit: 0,
              total_rows: 1,
              returned_rows: 0,
              mode: 'distinct',
              has_more: false,
              distinct: { precursor_mz: [75, 81] },
            }
          : {
              result_id: 'chromatograms-1',
              columns: [],
              rows: [row],
              offset: 0,
              limit: 1,
              total_rows: 1,
              returned_rows: 1,
              mode: request.mode,
              has_more: false,
            },
      ),
    );
    const client = { resultQuery } as unknown as StreamFindApiClient;
    const context: ViewerContext = {
      sessionId: 'session-1',
      resultId: 'chromatograms-1',
      semanticType: 'chromatogramsTable',
      artifact,
      pluginApi: { client } as FrontendPluginApi,
    };

    render(<ChromatogramsTableViewer context={context} />);
    await waitFor(() => expect(screen.getByTestId('chromatogram-plot')).toBeInTheDocument());

    fireEvent.click(screen.getByLabelText('Filter by Precursor m/z'));
    fireEvent.click(screen.getByRole('checkbox', { name: '75' }));
    fireEvent.click(screen.getByRole('checkbox', { name: '81' }));
    await waitFor(() =>
      expect(
        resultQuery.mock.calls.some(
          ([, request]) =>
            request.mode === 'page' &&
            request.filters?.length === 1 &&
            request.filters[0].column === 'precursor_mz' &&
            request.filters[0].operator === 'between' &&
            request.filters[0].min < 75 &&
            request.filters[0].max > 81,
        ),
      ).toBe(true),
    );
  });
});
