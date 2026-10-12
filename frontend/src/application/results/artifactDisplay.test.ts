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
});
