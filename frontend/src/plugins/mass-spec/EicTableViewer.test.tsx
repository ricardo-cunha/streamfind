import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ResultRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../../framework/plugins/pluginTypes';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerContext } from '../../framework/viewers/viewerTypes';
import { EicTableViewer } from './EicTableViewer';

vi.mock('../../framework/visualization/VisualizationRenderer', () => ({
  VisualizationRenderer: ({ spec }: { spec: VisualizationSpec }) => (
    <output data-testid="eic-plot">{JSON.stringify(spec)}</output>
  ),
}));

const artifact: ResultRecord = {
  result_id: 'eic-1',
  contract_id: 'eicTable',
  representation: 'table',
  producer_operation: 'mass_spec.get_raw_spectra_eic',
  producer_instance: 'node-21',
  workflow_revision: 1,
  status: 'published',
};

const row = (analysis: string) => ({
  analysis,
  name: 'target',
  replicate: '1',
  polarity: 'pos',
  level: '1',
  rt: '5',
  intensity: '100',
});

afterEach(() => cleanup());

describe('EicTableViewer', () => {
  it('reloads filtered rows from the service instead of filtering only the initial sample', async () => {
    const resultQuery = vi.fn().mockImplementation((_sessionId, request) => {
      if (request.mode === 'distinct')
        return Promise.resolve({
          result_id: 'eic-1',
          columns: [],
          rows: [],
          offset: 0,
          limit: 0,
          total_rows: 2,
          returned_rows: 0,
          mode: 'distinct',
          has_more: false,
          distinct: { analysis: ['sample-1', 'sample-2'] },
        });
      if (request.mode === 'page')
        return Promise.resolve({
          result_id: 'eic-1',
          columns: [],
          rows: [row('sample-2')],
          offset: 0,
          limit: 1,
          total_rows: 1,
          returned_rows: 1,
          mode: 'page',
          has_more: false,
        });
      return Promise.resolve({
        result_id: 'eic-1',
        columns: [],
        rows: [row('sample-1')],
        offset: 0,
        limit: 1,
        total_rows: 2,
        returned_rows: 1,
        mode: 'eic',
        has_more: true,
      });
    });
    const client = { resultQuery } as unknown as StreamFindApiClient;
    const context: ViewerContext = {
      sessionId: 'session-1',
      resultId: 'eic-1',
      semanticType: 'eicTable',
      artifact,
      pluginApi: { client } as FrontendPluginApi,
    };

    render(<EicTableViewer context={context} />);
    await waitFor(() => expect(screen.getByTestId('eic-plot')).toBeInTheDocument());
    expect(screen.getByTestId('eic-plot').textContent).toContain('sample-1');

    fireEvent.click(screen.getByLabelText('Filter by Analysis'));
    fireEvent.click(screen.getByRole('checkbox', { name: 'sample-2' }));

    await waitFor(() =>
      expect(
        resultQuery.mock.calls.some(
          ([, request]) => request.mode === 'page' && request.filters?.[0]?.column === 'analysis',
        ),
      ).toBe(true),
    );
    await waitFor(() => expect(screen.getByTestId('eic-plot').textContent).toContain('sample-2'));
  });
});
