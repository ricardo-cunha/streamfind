import { cleanup, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ResultRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../../framework/plugins/pluginTypes';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerContext } from '../../framework/viewers/viewerTypes';
import { ChromatogramPeaksViewer } from './ChromatogramPeaksViewer';

vi.mock('../../framework/visualization/VisualizationRenderer', () => ({
  VisualizationRenderer: ({ spec }: { spec: VisualizationSpec }) => (
    <output data-testid="peak-plot">{JSON.stringify(spec)}</output>
  ),
}));

const artifact: ResultRecord = {
  result_id: 'peaks-1',
  contract_id: 'chromatogramPeaksTable',
  representation: 'table',
  producer_operation: 'mass_spec.find_chromatogram_peaks',
  producer_instance: 'node-3',
  workflow_revision: 1,
  status: 'published',
};

const row = {
  analysis: 'sample-1',
  chromatogram_id: 'SRM SIC Q1=81 Q3=46 name=NDMA-d6',
  chromatogram_index: '0',
  peak_id: '1',
  channel: 'NDMA-d6',
  polarity: '1',
  signal_type: 'SRM',
  chromatogram_type: 'SIC',
  precursor_mz: '81',
  product_mz: '46',
  rt: '5',
  rt_start: '4',
  rt_end: '6',
  height: '100',
  area: '200',
  eic_rt: JSON.stringify([4, 5, 6]),
  eic_intensity: JSON.stringify([0, 100, 0]),
  eic_raw_intensity: JSON.stringify([0, 110, 0]),
  eic_baseline: JSON.stringify([0, 0, 0]),
};

afterEach(() => cleanup());

describe('ChromatogramPeaksViewer', () => {
  it('queries the peak-table schema and creates an EIC peak-window plot', async () => {
    const resultQuery = vi.fn().mockResolvedValue({
      result_id: 'peaks-1',
      columns: [],
      rows: [row],
      offset: 0,
      limit: 1,
      total_rows: 1,
      returned_rows: 1,
      mode: 'sample',
      has_more: false,
    });
    const client = { resultQuery } as unknown as StreamFindApiClient;
    const context: ViewerContext = {
      sessionId: 'session-1',
      resultId: 'peaks-1',
      semanticType: 'chromatogramPeaksTable',
      artifact,
      pluginApi: { client } as FrontendPluginApi,
    };

    render(<ChromatogramPeaksViewer context={context} />);

    await waitFor(() => expect(screen.getByTestId('peak-plot')).toBeInTheDocument());
    const request = resultQuery.mock.calls[0][1];
    expect(request.columns).not.toContain('name');
    expect(request.columns).toEqual(
      expect.arrayContaining(['chromatogram_id', 'precursor_mz', 'product_mz', 'eic_rt']),
    );
    expect(screen.getByTestId('peak-plot').textContent).toMatch(/"fillcolor":"rgba\(/);
  });
});
