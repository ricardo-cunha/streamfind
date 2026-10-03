import { describe, expect, it } from 'vitest';
import { artifactContractMatches, localArtifactContract } from './artifactContracts';

describe('artifact contracts', () => {
  it('normalizes full semantic IRIs and prefixed names to local contracts', () => {
    expect(localArtifactContract('https://streamfind.dev/semantic#featuresTable')).toBe('featuresTable');
    expect(localArtifactContract('sf:featuresTable')).toBe('featuresTable');
  });

  it('matches equivalent qualified and local contract identifiers', () => {
    expect(artifactContractMatches('sf:featuresTable', 'featuresTable')).toBe(true);
    expect(artifactContractMatches('sf:featuresTable', 'sf:analysesTable')).toBe(false);
  });
});
