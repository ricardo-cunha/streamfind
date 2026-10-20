import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ArtifactRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import { FrontendPluginRegistry } from '../../framework/plugins/FrontendPluginRegistry';
import { ViewerRegistry } from '../../framework/viewers/viewerTypes';
import { coreFrontendPlugin } from '../core';
import {
  STATISTICS_ARTIFACT_CONTRACTS,
  STATISTICS_RESULT_CONTRACTS,
  STATISTICS_TABLE_CONTRACTS,
  buildStatisticsPanels,
  getStatisticsSelection,
} from './statisticsPlots';
import { statisticsFrontendPlugin } from './index';
import { StatisticsDashboard } from './StatisticsDashboard';

function artifact(contract: string, artifactId = 'artifact-1'): ArtifactRecord {
  return {
    artifact_id: artifactId,
    contract_id: contract,
    representation: contract.endsWith('Table') ? 'table' : 'json',
    producer_operation: 'statistics.pca',
    producer_instance: 'node-statistics-1',
    workflow_revision: 1,
    status: 'current',
    payload: null,
  };
}

function viewerContext(
  selected: ArtifactRecord,
  currentArtifacts: ReturnType<typeof vi.fn>,
  artifactQuery: ReturnType<typeof vi.fn>,
) {
  return {
    sessionId: 'session-1',
    artifactId: selected.artifact_id,
    semanticType: selected.contract_id,
    artifact: selected,
    pluginApi: { client: { currentArtifacts, artifactQuery } as unknown as StreamFindApiClient },
  } as never;
}

describe('statistics frontend plugin', () => {
  it('leaves table outputs to the generic table renderer and registers plots for structured results', () => {
    const viewers = new ViewerRegistry();
    const plugins = new FrontendPluginRegistry(viewers);
    plugins.register(coreFrontendPlugin);
    plugins.register(statisticsFrontendPlugin);

    expect(plugins.providersForArtifact('sfstat:pcaScoresTable')).toEqual([]);
    expect(STATISTICS_ARTIFACT_CONTRACTS).toHaveLength(11);
    for (const contract of STATISTICS_TABLE_CONTRACTS)
      expect(viewers.resolve({ semanticType: contract, representation: 'table' })?.id).toBe('core.table');
    for (const contract of STATISTICS_RESULT_CONTRACTS)
      expect(viewers.resolve(contract)?.id).toBe('statistics.results-dashboard');
  });

  it('builds bounded Plotly panels with selectable sample and variable IDs', () => {
    const panels = buildStatisticsPanels(
      {
        pcaScoresTable: [
          { sample_id: 's1', component: '1', score: '-1', group_label: 'A' },
          { sample_id: 's1', component: '2', score: '0.5', group_label: 'A' },
          { sample_id: 's2', component: '1', score: '1', group_label: 'B' },
          { sample_id: 's2', component: '2', score: '-0.5', group_label: 'B' },
        ],
        pcaLoadingsTable: [
          { variable_id: 'v1', component: '1', loading: '0.8' },
          { variable_id: 'v1', component: '2', loading: '0.1' },
        ],
        pcaVarianceTable: [{ component: '1', explained_variance_ratio: '0.8' }],
        foldChangeTable: [{ variable_id: 'v1', log2_fold_change: '1.5', p_value: '0.01', significant: 'true' }],
      },
      'artifact-1',
      { text: '#111', grid: '#ddd', background: '#fff', accent: '#39f' },
      's1',
    );

    expect(panels.map((panel) => panel.id)).toEqual([
      'pca-scores',
      'pca-loadings',
      'pca-variance',
      'fold-change-volcano',
    ]);
    expect(panels[0].spec.payload.data[0].customdata).toEqual(['s1']);
    expect(panels[0].spec.payload.data[0].marker).toMatchObject({ size: [13] });
    expect(panels[1].spec.payload.data[0].customdata).toEqual(['v1']);
    expect(panels[1].spec.payload.data[0].textposition).toBe('top center');
    expect(panels[3].spec.payload.data[0].y).toEqual([2]);
    expect(panels[0].spec.payload.layout?.paper_bgcolor).toBe('#fff');
    expect(panels[0].spec.payload.layout?.legend).toMatchObject({
      orientation: 'h',
      x: 0,
      y: -0.12,
      yanchor: 'top',
    });
    expect(getStatisticsSelection({ customdata: ['sample-7', 'class-a'] })).toBe('sample-7');
  });

  it('does not render a second table inside the plot viewer', async () => {
    const summaryArtifact = artifact('foldChangeDiagnostics', 'summary-1');
    const tableArtifact = artifact('foldChangeTable', 'table-1');
    const currentArtifacts = vi.fn().mockResolvedValue([summaryArtifact, tableArtifact]);
    const artifactQuery = vi.fn().mockResolvedValue({
      artifact_id: tableArtifact.artifact_id,
      columns: [{ name: 'variable_id', type: 'VARCHAR' }],
      rows: [{ variable_id: 'v1', log2_fold_change: '1', p_value: '0.01', significant: 'true' }],
      offset: 0,
      limit: 5000,
      total_rows: 1,
      returned_rows: 1,
      mode: 'page',
      has_more: false,
    });
    render(<StatisticsDashboard context={viewerContext(summaryArtifact, currentArtifacts, artifactQuery)} />);

    expect(await screen.findByRole('region', { name: 'Statistics plot' })).toBeInTheDocument();
    expect(screen.getByRole('option', { name: 'Fold-change volcano plot' })).toBeInTheDocument();
    expect(screen.queryByText('Result table')).not.toBeInTheDocument();
  });

  it('shows an actionable error when artifact loading fails', async () => {
    const selected = artifact('pcaDiagnostics');
    const currentArtifacts = vi.fn().mockRejectedValue(new Error('service unavailable'));
    const artifactQuery = vi.fn();
    render(<StatisticsDashboard context={viewerContext(selected, currentArtifacts, artifactQuery)} />);

    expect(await screen.findByRole('alert')).toHaveTextContent('service unavailable');
  });

  it('renders an empty-result message when only diagnostics are available', async () => {
    const selected = { ...artifact('pcaDiagnostics'), payload: { method: 'pca' } };
    const currentArtifacts = vi.fn().mockResolvedValue([selected]);
    const artifactQuery = vi.fn();
    render(<StatisticsDashboard context={viewerContext(selected, currentArtifacts, artifactQuery)} />);

    expect(await screen.findByText('No plot-ready statistics results are available.')).toBeInTheDocument();
  });
});
