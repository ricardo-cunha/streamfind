import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ArtifactRecord, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import { buildFeatureNetwork, FeatureInspector } from './FeatureInspector';

import type { ViewerContext } from '../../framework/viewers/viewerTypes';
import type { FrontendPluginApi } from '../../framework/plugins/pluginTypes';
import { visualizationRegistry } from '../../framework/visualization/VisualizationRegistry';

const artifact: ArtifactRecord = {
  artifact_id: 'features-1',
  contract_id: 'sfms:featuresTable',
  representation: 'table',
  producer_operation: 'mass_spec.find_features',
  producer_instance: 'node-1',
  workflow_revision: 2,
  status: 'published',
};

afterEach(() => cleanup());

describe('FeatureInspector', () => {
  it('builds component networks from persisted partner and correlation columns', () => {
    const model = buildFeatureNetwork(
      [
        {
          feature: 'F1',
          analysis: 'a',
          feature_component: 'C1',
          feature_group: 'G1',
          mz: '100',
          rt: '10',
          component_best_partner: 'F2',
          component_max_correlation: '0.8',
        },
        {
          feature: 'F2',
          analysis: 'a',
          feature_component: 'C1',
          feature_group: 'G1',
          mz: '101',
          rt: '11',
          component_best_partner: 'F1',
          component_mean_correlation: '0.7',
        },
        { feature: 'F3', analysis: 'a', feature_component: 'C2', feature_group: 'G2', mz: '200', rt: '20' },
      ],
      'a\u0000C1',
      'feature_component',
    );

    expect(model.nodes.map((node) => node.id)).toEqual(['a\u0000F1', 'a\u0000F2']);
    expect(model.edges).toEqual([
      { source: 'a\u0000F1', target: 'a\u0000F2', weight: 0.8 },
      { source: 'a\u0000F2', target: 'a\u0000F1', weight: 0.7 },
    ]);
  });

  it('preserves explicit loss chains and requires a shared non-empty group for main ions', () => {
    const model = buildFeatureNetwork(
      [
        {
          feature: 'MAIN',
          feature_component: 'FC7_RT1233_POS',
          feature_group: 'G1',
          adduct: '[M+H]+',
          mz: '300',
          component_best_partner: 'MAIN-OTHER-GROUP',
        },
        {
          feature: 'MAIN-OTHER-GROUP',
          feature_component: 'FC7_RT1233_POS',
          feature_group: 'G2',
          adduct: '[M+H]+',
          mz: '301',
          component_best_partner: 'MAIN',
        },
        {
          feature: 'LOSS-1',
          feature_component: 'FC7_RT1233_POS',
          feature_group: 'G1',
          adduct: 'loss M-H2O',
          annotation_parent_feature: 'MAIN',
          mz: '282',
        },
        {
          feature: 'LOSS-2',
          feature_component: 'FC7_RT1233_POS',
          feature_group: 'G1',
          adduct: 'loss M-H2O',
          annotation_parent_feature: 'LOSS-1',
          mz: '264',
        },
      ],
      '\u0000FC7_RT1233_POS',
      'feature_component',
    );

    expect(model.edges).toContainEqual(expect.objectContaining({ source: '\u0000LOSS-1', target: '\u0000MAIN' }));
    expect(model.edges).toContainEqual(expect.objectContaining({ source: '\u0000LOSS-2', target: '\u0000LOSS-1' }));
    expect(model.edges).not.toContainEqual(expect.objectContaining({ source: '\u0000LOSS-1', target: '\u0000LOSS-2' }));
    expect(model.edges).not.toContainEqual(
      expect.objectContaining({ source: '\u0000MAIN', target: '\u0000MAIN-OTHER-GROUP' }),
    );
    expect(model.edges).not.toContainEqual(
      expect.objectContaining({ source: '\u0000MAIN-OTHER-GROUP', target: '\u0000MAIN' }),
    );
  });

  it('keeps feature-group selection cross-analysis', () => {
    const model = buildFeatureNetwork(
      [
        { feature: 'POS-F1', analysis: 'positive', feature_group: 'G42', adduct: '[M+H]+' },
        { feature: 'NEG-F1', analysis: 'negative', feature_group: 'G42', adduct: '[M-H]-' },
        { feature: 'OTHER', analysis: 'positive', feature_group: 'G99', adduct: '[M+H]+' },
      ],
      'G42',
      'feature_group',
    );

    expect(model.nodes.map((node) => node.id)).toEqual(['positive\u0000POS-F1', 'negative\u0000NEG-F1']);
  });

  it('collects every selected-analysis component member and preserves its loss chain', () => {
    const rows: Array<Record<string, string | null>> = [
      {
        feature: 'CL1017_PK242_MZ308_RT1231_POS',
        analysis: '01_tof_ww_is_pos_blank-r001',
        feature_component: 'FC7_RT1233_POS',
        adduct: 'loss M-H2O',
        annotation_type: 'M-H2O',
        annotation_parent_feature: 'CL1138_PK203_MZ326_RT1234_POS',
      },
      {
        feature: 'CL1138_PK203_MZ326_RT1234_POS',
        analysis: '01_tof_ww_is_pos_blank-r001',
        feature_component: 'FC7_RT1233_POS',
        adduct: 'loss M-H2O',
        annotation_type: 'M-H2O',
        annotation_parent_feature: 'CL1274_PK143_MZ344_RT1234_POS',
      },
      {
        feature: 'CL1274_PK143_MZ344_RT1234_POS',
        analysis: '01_tof_ww_is_pos_blank-r001',
        feature_component: 'FC7_RT1233_POS',
        adduct: '[M+H]+',
      },
      {
        feature: 'OTHER_ANALYSIS',
        analysis: '01_tof_ww_is_neg_blank-r001',
        feature_component: 'FC7_RT1233_POS',
        adduct: '[M-H]-',
      },
    ];
    const model = buildFeatureNetwork(rows, '01_tof_ww_is_pos_blank-r001\u0000FC7_RT1233_POS', 'feature_component');

    expect(model.nodes.map((node) => node.id)).toEqual([
      '01_tof_ww_is_pos_blank-r001\u0000CL1017_PK242_MZ308_RT1231_POS',
      '01_tof_ww_is_pos_blank-r001\u0000CL1138_PK203_MZ326_RT1234_POS',
      '01_tof_ww_is_pos_blank-r001\u0000CL1274_PK143_MZ344_RT1234_POS',
    ]);
    expect(model.edges).toEqual([
      {
        source: '01_tof_ww_is_pos_blank-r001\u0000CL1017_PK242_MZ308_RT1231_POS',
        target: '01_tof_ww_is_pos_blank-r001\u0000CL1138_PK203_MZ326_RT1234_POS',
        weight: 0,
      },
      {
        source: '01_tof_ww_is_pos_blank-r001\u0000CL1138_PK203_MZ326_RT1234_POS',
        target: '01_tof_ww_is_pos_blank-r001\u0000CL1274_PK143_MZ344_RT1234_POS',
        weight: 0,
      },
    ]);
  });

  it('loads feature rows, selects a feature, and queries the native EIC operation', async () => {
    visualizationRegistry.register('core.plotly', ({ onPointClick }) => (
      <svg role="img" aria-label="Feature scatter plot">
        <circle onClick={() => onPointClick?.({ pointNumber: 0, customdata: 'incorrect-transported-key' })} />
      </svg>
    ));
    const artifactData = vi.fn().mockResolvedValue({
      artifact_id: artifact.artifact_id,
      columns: [],
      rows: [
        {
          feature_id: 'F1',
          analysis: 'sample-a',
          mz: '275.2',
          rt: '12.5',
          eic_size: '2',
          eic_rt: 'AACAPwAAAEA=',
          eic_intensity: 'AADIQgAASEM=',
        },
        { feature_id: 'F2', analysis: 'sample-b', mz: '300.1', rt: '18.0' },
      ],
      offset: 0,
      limit: 250,
      total_rows: 2,
    });
    const api = { artifactData } as unknown as StreamFindApiClient;
    const pluginApi = { client: api } as FrontendPluginApi;
    const context: ViewerContext = {
      sessionId: 'session-1',
      artifactId: artifact.artifact_id,
      semanticType: artifact.contract_id,
      artifact,
      pluginApi,
    };

    render(<FeatureInspector context={context} />);
    await waitFor(() => expect(screen.getByRole('img', { name: 'Feature scatter plot' })).toBeInTheDocument());
    const point = document.querySelector('circle');
    expect(point).not.toBeNull();
    fireEvent.click(point as Element);
    fireEvent.click(screen.getByRole('tab', { name: 'EIC' }));
    await waitFor(() => expect(screen.queryByText(/No encoded EIC data/)).not.toBeInTheDocument());
  });

  it('loads all artifact pages and filters empty component values in component mode', async () => {
    if (!visualizationRegistry.has('core.plotly'))
      visualizationRegistry.register('core.plotly', () => <svg role="img" aria-label="Feature scatter plot" />);
    const artifactData = vi.fn().mockImplementation(async (_sessionId: string, request: { offset: number }) => {
      if (request.offset === 0) {
        return {
          artifact_id: artifact.artifact_id,
          columns: [],
          rows: [{ feature_id: 'F1', analysis: 'sample-a', feature_component: '', mz: '275.2', rt: '12.5' }],
          offset: 0,
          limit: 1000,
          total_rows: 2,
        };
      }
      return {
        artifact_id: artifact.artifact_id,
        columns: [],
        rows: [{ feature_id: 'F2', analysis: 'sample-a', feature_component: 'C1', mz: '300.1', rt: '18.0' }],
        offset: 1,
        limit: 1000,
        total_rows: 2,
      };
    });
    const api = { artifactData } as unknown as StreamFindApiClient;
    const pluginApi = { client: api } as FrontendPluginApi;

    render(
      <FeatureInspector
        context={{
          sessionId: 'session-1',
          artifactId: artifact.artifact_id,
          semanticType: artifact.contract_id,
          artifact,
          pluginApi,
        }}
      />,
    );

    await waitFor(() => expect(screen.getByText('Features 2 / 2')).toBeInTheDocument());
    fireEvent.change(screen.getByLabelText('Select by'), { target: { value: 'feature_component' } });
    expect(screen.getByText('Features 1 / 2')).toBeInTheDocument();
    expect(artifactData).toHaveBeenNthCalledWith(1, 'session-1', expect.objectContaining({ offset: 0, limit: 1000 }));
    expect(artifactData).toHaveBeenNthCalledWith(2, 'session-1', expect.objectContaining({ offset: 1, limit: 1000 }));
  });
});
