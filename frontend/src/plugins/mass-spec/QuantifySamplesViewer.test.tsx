import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ResultRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../../framework/plugins/pluginTypes';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerContext } from '../../framework/viewers/viewerTypes';
import { QuantifySamplesViewer } from './QuantifySamplesViewer';

vi.mock('../../framework/visualization/VisualizationRenderer', () => ({
  VisualizationRenderer: ({ spec }: { spec: VisualizationSpec }) => (
    <output data-testid="quantification-plot">{JSON.stringify(spec)}</output>
  ),
}));

const contracts = [
  'quantificationCalibrationModelsTable',
  'quantificationTable',
] as const;

const artifacts = contracts.map((contract, index) => ({
  result_id: `quant-${index}`,
  contract_id: contract,
  representation: 'table' as const,
  producer_operation: 'mass_spec.quantify',
  producer_instance: 'node-5',
  workflow_revision: 1,
  status: 'published' as const,
}));

afterEach(() => cleanup());

describe('QuantifySamplesViewer', () => {
  it('loads sibling quantification tables and switches to result limits', async () => {
    const rows: Record<string, Record<string, string | null>[]> = {
      quantificationCalibrationModelsTable: [{
        target_id: 'diclofenac', model: 'linear', coefficient_0: '1', coefficient_1: '2',
        r_squared: '0.99', fit_status: 'ok', range_min: '1', range_max: '10',
      }],
      quantificationTable: [{
        analysis: 'cal-1', target_id: 'diclofenac', role: 'analyte', sample_type: 'calibrator', nominal_concentration: '5', response_ratio: '11', included: 'true', residual: '0.5', status: 'matched', eic_rt: '[1,2,3]', eic_intensity: '[0,10,0]', qualifier_eic_rt: '[1,2,3]', qualifier_eic_intensity: '[0,5,0]',
      }, {
        analysis: 'sample-1', target_id: 'diclofenac', sample_type: 'unknown', peak_rt: '378',
        role: 'analyte', nominal_concentration: '10', response_ratio: '15', final_concentration: '12', accuracy_percent: '130', status: 'ok', flags: 'above_calibration_range',
      }, {
        analysis: 'qc-1', target_id: 'diclofenac', sample_type: 'qc', peak_rt: '379',
        role: 'analyte', nominal_concentration: '10', response_ratio: '14', final_concentration: '10', accuracy_percent: '100', status: 'ok',
      }, {
        analysis: 'blank-1', target_id: 'diclofenac', sample_type: 'blank', peak_rt: '380',
        role: 'analyte', response_ratio: '0', final_concentration: '0', status: 'ok',
      }],
    };
    const resultQuery = vi.fn(async (_sessionId: string, request: { result_id: string; columns?: string[] }) => {
      const artifact = artifacts.find((candidate) => candidate.result_id === request.result_id);
      const contract = artifact?.contract_id ?? 'quantificationTable';
      return { result_id: request.result_id, columns: [], rows: rows[contract], offset: 0, limit: 10000, total_rows: rows[contract].length, returned_rows: rows[contract].length, mode: 'page' as const, has_more: false };
    });
    const client = {
      currentResults: vi.fn().mockResolvedValue(artifacts),
      resultQuery,
    } as unknown as StreamFindApiClient;
    const context: ViewerContext = {
      sessionId: 'session-1',
      resultId: 'quant-1',
      semanticType: 'quantificationTable',
      artifact: artifacts[1] as ResultRecord,
      pluginApi: { client } as FrontendPluginApi,
    };

    render(<QuantifySamplesViewer context={context} />);

    await waitFor(() => expect(screen.getAllByTestId('quantification-plot')).toHaveLength(2));
    expect(resultQuery.mock.calls.some(([, request]) => request.columns?.includes('eic_rt'))).toBe(true);
    expect(resultQuery.mock.calls.some(([, request]) => request.columns?.includes('qualifier_eic_rt'))).toBe(true);
    expect(screen.getByRole('img', { name: 'Quantifier EIC peak shape' })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Qualifier EIC peak shape' })).toBeInTheDocument();
    expect(screen.getByText('Calibration models')).toBeInTheDocument();
    expect(screen.getAllByTestId('quantification-plot')[1].textContent).toContain('Residual');
    fireEvent.click(screen.getByRole('tab', { name: 'Responses & results' }));
    expect(screen.getByText('QC, unknown & blank results')).toBeInTheDocument();
    expect(screen.queryByText('cal-1')).not.toBeInTheDocument();
    expect(screen.getByText('above limit')).toBeInTheDocument();
    expect(screen.getByTestId('quantification-plot').textContent).toContain('error_y');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('lower calibration limit');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('diamond');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('circle-open');
  });
});
