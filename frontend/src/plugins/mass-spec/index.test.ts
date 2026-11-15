import { describe, expect, it } from 'vitest';
import { FrontendPluginRegistry } from '../../framework/plugins/FrontendPluginRegistry';
import { ViewerRegistry } from '../../framework/viewers/viewerTypes';
import { massSpecFrontendPlugin } from './index';

describe('mass-spec frontend plugin', () => {
  it('declares and registers the chromatograms-table viewer contract', () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);

    registry.register(massSpecFrontendPlugin);

    expect(massSpecFrontendPlugin.manifest.artifactContracts).toContain('chromatogramsTable');
    expect(viewers.resolve({ semanticType: 'chromatogramsTable', representation: 'table' })?.id).toBe(
      'mass-spec.chromatograms-table-viewer',
    );
  });

  it('registers the dedicated quantification viewer for the unified table and models', () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    registry.register(massSpecFrontendPlugin);

    for (const contract of ['quantificationTable', 'quantificationCalibrationModelsTable']) {
      expect(viewers.resolve({ semanticType: contract, representation: 'table' })?.id).toBe(
        'mass-spec.quantification-viewer',
      );
    }
  });
});
