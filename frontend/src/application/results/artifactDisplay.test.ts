import { describe, expect, it } from 'vitest';
import type { ResultRecord } from '../../framework/backend/StreamFindApiClient';
import { artifactContractMatches, artifactSummary, prettyArtifactPayload } from './artifactDisplay';

const artifact: ResultRecord = {
  result_id: 'artifact-1',
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

  it('matches qualified artifact contracts', () => {
    expect(artifactContractMatches('sf:table', 'table')).toBe(true);
  });

  it('summarizes JSON artifacts without exposing their raw payload', () => {
    const summary: ResultRecord = {
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
    expect(artifactSummary(summary)).toBe('JSON\nObject · 4 fields');
  });

  it('summarizes debug JSON generically without rendering log content', () => {
    const debug: ResultRecord = {
      ...artifact,
      contract_id: 'sfmass:findFeaturesDebugResult',
      representation: 'json',
      payload: JSON.stringify({ analysis: 'sample', enabled: true, log_path: 'debug.txt', log_text: 'long log' }),
    };
    expect(artifactSummary(debug)).toBe('JSON\nObject · 4 fields');
    expect(artifactSummary(debug)).not.toContain('long log');
  });
});
