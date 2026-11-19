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

const contracts = ['quantificationCalibrationModelsTable', 'quantificationTable'] as const;

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
      quantificationCalibrationModelsTable: [
        {
          target_id: 'diclofenac',
          model: 'quadratic',
          coefficient_0: '1',
          coefficient_1: '2',
          coefficient_2: '0.1',
          r_squared: '0.99',
          fit_status: 'ok',
          monotonicity_status: 'increasing',
          range_min: '1',
          range_max: '10',
        },
      ],
      quantificationTable: [
        {
          analysis: 'cal-1',
          target_id: 'diclofenac',
          role: 'analyte',
          sample_type: 'calibrator',
          nominal_concentration: '5',
          response_ratio: '11',
          included: 'true',
          residual: '0.5',
          status: 'matched',
          eic_rt: '[1,2,3]',
          eic_intensity: '[0,10,0]',
          peak_rt_start: '1.4',
          peak_rt_end: '2.6',
          qualifier_eic_rt: '[1,2,3]',
          qualifier_eic_intensity: '[0,5,0]',
          qualifier_peak_rt_start: '1.5',
          qualifier_peak_rt_end: '2.5',
          internal_standard_eic_rt: '[1,2,3]',
          internal_standard_eic_intensity: '[0,7,0]',
          internal_standard_peak_rt_start: '1.3',
          internal_standard_peak_rt_end: '2.7',
        },
        {
          analysis: 'sample-1',
          target_id: 'diclofenac',
          sample_type: 'unknown',
          peak_rt: '378',
          role: 'analyte',
          nominal_concentration: '10',
          eic_rt: '[377.8,378,378.2]',
          eic_intensity: '[0,100,0]',
          peak_rt_start: '377.9',
          peak_rt_end: '378.1',
          qualifier_eic_rt: '[377.8,378,378.2]',
          qualifier_eic_intensity: '[0,45,0]',
          qualifier_peak_rt_start: '377.9',
          qualifier_peak_rt_end: '378.1',
          internal_standard_eic_rt: '[377.8,378,378.2]',
          internal_standard_eic_intensity: '[0,75,0]',
          internal_standard_peak_rt_start: '377.85',
          internal_standard_peak_rt_end: '378.15',
          chromatogram_index: '20',
          peak_id: '101',
          peak_area: '12345',
          peak_height: '100',
          quantifier_chromatogram_id: 'quantifier-channel',
          qualifier_chromatogram_id: 'qualifier-channel',
          qualifier_chromatogram_index: '21',
          qualifier_peak_id: '102',
          qualifier_peak_rt: '377.99',
          qualifier_peak_area: '5432',
          qualifier_peak_height: '45',
          qualifier_rt_error: '-0.01',
          qualifier_candidate_count: '1',
          response: '12345',
          internal_standard_response: '75',
          qualifier_response: '5432',
          qualifier_ratio: '0.44',
          qualifier_status: 'pass',
          internal_standard_status: 'matched',
          response_ratio: '15',
          final_concentration: '12',
          accuracy_percent: '130',
          status: 'ok',
          flags: 'above_calibration_range',
        },
        {
          analysis: 'qc-1',
          target_id: 'diclofenac',
          sample_type: 'qc',
          peak_rt: '379',
          role: 'analyte',
          nominal_concentration: '10',
          response_ratio: '14',
          final_concentration: '10',
          accuracy_percent: '100',
          status: 'ok',
        },
        {
          analysis: 'blank-1',
          target_id: 'diclofenac',
          sample_type: 'blank',
          peak_rt: '380',
          role: 'analyte',
          response_ratio: '0',
          final_concentration: '0',
          status: 'ok',
        },
        {
          analysis: 'sample-2',
          target_id: 'sulfamethoxazol',
          sample_type: 'unknown',
          peak_rt: '176',
          role: 'analyte',
          response_ratio: '8',
          final_concentration: '6',
          status: 'ok',
        },
        {
          analysis: 'sample-1',
          target_id: 'diclofenac_d4',
          role: 'internal_standard',
          sample_type: 'unknown',
          eic_rt: '[1,2,3]',
          eic_intensity: '[0,8,0]',
        },
      ],
    };
    const resultQuery = vi.fn(async (_sessionId: string, request: { result_id: string; columns?: string[] }) => {
      const artifact = artifacts.find((candidate) => candidate.result_id === request.result_id);
      const contract = artifact?.contract_id ?? 'quantificationTable';
      return {
        result_id: request.result_id,
        columns: [],
        rows: rows[contract],
        offset: 0,
        limit: 10000,
        total_rows: rows[contract].length,
        returned_rows: rows[contract].length,
        mode: 'page' as const,
        has_more: false,
      };
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
    expect(resultQuery.mock.calls.some(([, request]) => request.columns?.includes('internal_standard_eic_rt'))).toBe(
      true,
    );
    expect(resultQuery.mock.calls.some(([, request]) => request.columns?.includes('peak_rt_start'))).toBe(true);
    expect(resultQuery.mock.calls.some(([, request]) => request.columns?.includes('qualifier_peak_rt_end'))).toBe(true);
    expect(screen.getByRole('img', { name: 'Quantifier EIC peak shape' })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Qualifier EIC peak shape' })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Internal standard EIC peak shape' })).toBeInTheDocument();
    expect(screen.getByText('Calibration models')).toBeInTheDocument();
    expect(screen.getAllByTestId('quantification-plot')[1].textContent).toContain('Residual');
    const calibrationPlot = screen.getAllByTestId('quantification-plot')[0].textContent ?? '';
    expect(calibrationPlot).toContain('quadratic fit');
    expect(calibrationPlot).toContain('"name":"diclofenac"');
    expect(calibrationPlot).not.toContain('"name":"diclofenac raw"');
    expect(calibrationPlot).not.toContain('"name":"diclofenac excluded"');
    expect(calibrationPlot).not.toContain('"name":"diclofenac quadratic fit"');
    fireEvent.click(screen.getByRole('tab', { name: 'Responses & results' }));
    expect(screen.getByText('QC, unknown & blank results')).toBeInTheDocument();
    expect(screen.queryByText('cal-1')).not.toBeInTheDocument();
    expect(screen.getByLabelText('Filter by Compound')).toBeInTheDocument();
    expect(screen.getByLabelText('Filter by Sample type')).toBeInTheDocument();
    fireEvent.click(screen.getByLabelText('Filter by Compound'));
    fireEvent.click(screen.getByLabelText('sulfamethoxazol'));
    fireEvent.click(screen.getByLabelText('diclofenac'));
    expect(screen.getByText('2 selected')).toBeInTheDocument();
    expect(screen.getByText('sample-2')).toBeInTheDocument();
    expect(screen.getByText('sample-1')).toBeInTheDocument();
    fireEvent.click(screen.getByLabelText('Filter by Sample type'));
    fireEvent.click(screen.getByLabelText('qc'));
    fireEvent.click(screen.getByLabelText('unknown'));
    fireEvent.click(screen.getByLabelText('blank'));
    expect(screen.getByText('3 selected')).toBeInTheDocument();
    expect(screen.getByText('above limit')).toBeInTheDocument();
    expect(screen.queryByText('diclofenac_d4')).not.toBeInTheDocument();
    expect(screen.getByTestId('quantification-plot').textContent).toContain('error_y');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('lower calibration limit');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('diamond');
    expect(screen.getByTestId('quantification-plot').textContent).toContain('circle-open');
    const resultRow = screen.getByText('sample-1').closest('tr');
    expect(resultRow).not.toBeNull();
    fireEvent.doubleClick(resultRow as HTMLElement);
    expect(screen.getByRole('dialog', { name: 'Quantification row details' })).toBeInTheDocument();
    expect(screen.getByText('Peak inspection')).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Quantifier EIC detail' })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Qualifier EIC detail' })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Internal standard EIC detail' })).toBeInTheDocument();
    expect(screen.getAllByText('Integration start RT').length).toBeGreaterThanOrEqual(2);
    expect(screen.getAllByText('Integration end RT').length).toBeGreaterThanOrEqual(2);
    expect(document.querySelectorAll('.is-boundary')).toHaveLength(6);
    expect(screen.getByText('quantifier-channel')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Close row details' }));
    expect(screen.queryByRole('dialog', { name: 'Quantification row details' })).not.toBeInTheDocument();
  });
});
