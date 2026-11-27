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

  it('pretty-prints typed JSON values without parsing JSON-looking strings', () => {
    expect(prettyArtifactPayload({ value: 1 })).toContain('"value": 1');
    expect(JSON.parse(prettyArtifactPayload('{"value":1}'))).toBe('{"value":1}');
    expect(JSON.parse(prettyArtifactPayload('raw text'))).toBe('raw text');
  });

  it('matches qualified artifact contracts', () => {
    expect(artifactContractMatches('sf:table', 'table')).toBe(true);
  });

  it('summarizes JSON artifacts without exposing their raw payload', () => {
    const payload = JSON.stringify({
      samples_group_a: 3,
      samples_group_b: 4,
      variables_tested: 11192,
      significant_variables: 27,
    });
    const summary: ResultRecord = {
      ...artifact,
      contract_id: 'sfstat:foldChangeDiagnostics',
      representation: 'json',
      payload,
    };
    expect(artifactSummary(summary)).toBe(`JSON\nString · ${payload.length} characters`);
  });

  it('summarizes metadata-only JSON descriptors without requiring payload hydration', () => {
    expect(artifactSummary({ ...artifact, representation: 'json', payload: undefined })).toBe('JSON result · open to view');
  });

  it('summarizes debug JSON generically without rendering log content', () => {
    const payload = JSON.stringify({ analysis: 'sample', enabled: true, log_path: 'debug.txt', log_text: 'long log' });
    const debug: ResultRecord = {
      ...artifact,
      contract_id: 'sfmass:findFeaturesDebugResult',
      representation: 'json',
      payload,
    };
    expect(artifactSummary(debug)).toBe(`JSON\nString · ${payload.length} characters`);
    expect(artifactSummary(debug)).not.toContain('long log');
  });
});
