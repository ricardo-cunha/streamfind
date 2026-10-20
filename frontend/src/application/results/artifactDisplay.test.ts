import { describe, expect, it } from 'vitest';
import type { ArtifactRecord } from '../../framework/backend/StreamFindApiClient';
import {
  artifactContractMatches,
  artifactSummary,
  isVisualizationArtifact,
  prettyArtifactPayload,
} from './artifactDisplay';

const artifact: ArtifactRecord = {
  artifact_id: 'artifact-1',
  contract_id: 'sf:table',
  representation: 'table',
  columns: [{ name: 'mz', type: 'double' }],
  row_count: 2,
  producer_operation: 'operation',
  producer_instance: 'instance',
  workflow_revision: 1,
  status: 'completed',
};

describe('artifact display model', () => {
  it('summarizes table artifacts without loading payload data', () => {
    expect(artifactSummary(artifact)).toContain('1 columns · 2 rows');
  });

  it('pretty-prints JSON payloads and preserves invalid strings', () => {
    expect(prettyArtifactPayload('{"value":1}')).toContain('"value": 1');
    expect(prettyArtifactPayload('raw text')).toBe('raw text');
  });

  it('matches qualified artifact contracts and identifies visualizations', () => {
    expect(artifactContractMatches('sf:table', 'table')).toBe(true);
    expect(isVisualizationArtifact({ ...artifact, contract_id: 'sf:visualizationSpecResult' })).toBe(true);
  });

  it('summarizes visualization artifacts without exposing the full plot payload', () => {
    const visualization: ArtifactRecord = {
      ...artifact,
      contract_id: 'sf:visualizationSpecResult',
      representation: 'json',
      payload: JSON.stringify({ payload: { data: [{ name: 'sample-a', x: [1, 2, 3], y: [4, 5, 6] }] } }),
    };
    expect(artifactSummary(visualization)).toBe('JSON\n1 legend traces · 3 total points · trace length 3');
  });

  it('summarizes statistics result artifacts instead of exposing their raw JSON', () => {
    const summary: ArtifactRecord = {
      ...artifact,
      contract_id: 'sfstat:foldChangeDiagnostics',
      representation: 'json',
      payload: JSON.stringify({
        samples_group_a: 3,
        samples_group_b: 4,
        variables_tested: 11192,
        significant_variables: 27,
      }),
    };
    expect(artifactSummary(summary)).toBe(
      'JSON\n3 group A samples · 4 group B samples · 11,192 variables tested · 27 significant',
    );
  });
});
