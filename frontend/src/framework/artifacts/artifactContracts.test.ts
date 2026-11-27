import { describe, expect, it } from 'vitest';
import { artifactContractMatches, localArtifactContract } from './artifactContracts';

describe('artifact contracts', () => {
  it('normalizes full semantic IRIs and prefixed names to local contracts', () => {
    expect(localArtifactContract('https://streamfind.dev/semantic#featuresTable')).toBe('featuresTable');
    expect(localArtifactContract('sf:featuresTable')).toBe('featuresTable');
  });

  it('does not match a qualified contract to a local name', () => {
    expect(artifactContractMatches('sf:featuresTable', 'featuresTable')).toBe(false);
    expect(artifactContractMatches('sf:featuresTable', 'sf:analysesTable')).toBe(false);
  });

  it('requires exact qualified IRIs when local names collide', () => {
    expect(
      artifactContractMatches(
        'https://streamfind.dev/catalogue/plugins/alpha#result',
        'https://streamfind.dev/catalogue/plugins/beta#result',
      ),
    ).toBe(false);
    expect(
      artifactContractMatches(
        'https://streamfind.dev/catalogue/plugins/alpha#result',
        'https://streamfind.dev/catalogue/plugins/alpha#result',
      ),
    ).toBe(true);
  });
});
