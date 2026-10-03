import { describe, expect, it } from 'vitest';
import { StreamFindApiClient } from '../backend/StreamFindApiClient';
import { FrontendPluginRegistry } from './FrontendPluginRegistry';
import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from './pluginTypes';
import { ViewerRegistry } from '../viewers/viewerTypes';
import { VisualizationRegistry } from '../visualization/VisualizationRegistry';

function plugin(
  overrides: Partial<FrontendPlugin['manifest']> = {},
  setup: FrontendPlugin['setup'] = () => {},
): FrontendPlugin {
  return {
    manifest: {
      id: 'test.plugin',
      name: 'Test plugin',
      version: '1.0.0',
      apiVersion: FRONTEND_PLUGIN_API_VERSION,
      ...overrides,
    },
    setup,
  };
}

describe('frontend plugin registry', () => {
  it('registers a plugin viewer through the versioned API', () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers, new StreamFindApiClient('http://service'));

    registry.register(
      plugin({ artifactContracts: ['sf:table'] }, (api) => {
        api.registerViewer({
          id: 'test.table',
          label: 'Test table',
          accepts: ['table'],
          component: () => null,
        });
      }),
    );

    expect(registry.ids()).toEqual(['test.plugin']);
    expect(viewers.resolve('sf:table')?.id).toBe('test.table');
    expect(registry.providersForArtifact('sf:table')).toEqual(['test.plugin']);
  });

  it('rejects duplicate plugin IDs and unsupported API versions', () => {
    const registry = new FrontendPluginRegistry(new ViewerRegistry());
    registry.register(plugin());

    expect(() => registry.register(plugin())).toThrow('Frontend plugin already registered: test.plugin');
    expect(() =>
      registry.register(plugin({ id: 'other.plugin', apiVersion: '9.0' as typeof FRONTEND_PLUGIN_API_VERSION })),
    ).toThrow('Unsupported frontend plugin API version: 9.0');
  });

  it('rejects viewers outside the plugin semantic contract', () => {
    const registry = new FrontendPluginRegistry(new ViewerRegistry());

    expect(() =>
      registry.register(
        plugin({ artifactContracts: ['featuresTable'] }, (api) => {
          api.registerViewer({
            id: 'test.visualization',
            label: 'Wrong viewer',
            accepts: ['visualizationSpecResult'],
            component: () => null,
          });
        }),
      ),
    ).toThrow('Viewer contracts are incompatible with frontend plugin: test.visualization');
  });

  it('rolls back viewers when plugin setup fails', () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);

    expect(() =>
      registry.register(
        plugin({ artifactContracts: ['sf:table'] }, (api) => {
          api.registerViewer({
            id: 'test.partial',
            label: 'Partial viewer',
            accepts: ['sf:table'],
            component: () => null,
          });
          throw new Error('setup failed');
        }),
      ),
    ).toThrow('setup failed');

    expect(registry.has('test.plugin')).toBe(false);
    expect(viewers.has('test.partial')).toBe(false);
  });

  it('registers and rolls back visualization renderers through the public API', () => {
    const viewers = new ViewerRegistry();
    const visualizations = new VisualizationRegistry();
    const registry = new FrontendPluginRegistry(viewers, undefined, visualizations);
    const renderer = () => null;

    registry.register(
      plugin({ id: 'renderer.plugin' }, (api) => api.registerVisualizationRenderer('test.renderer', renderer)),
    );
    expect(visualizations.has('test.renderer')).toBe(true);

    expect(() =>
      registry.register(
        plugin({ id: 'failing.renderer.plugin' }, (api) => {
          api.registerVisualizationRenderer('partial.renderer', renderer);
          throw new Error('renderer setup failed');
        }),
      ),
    ).toThrow('renderer setup failed');
    expect(visualizations.has('partial.renderer')).toBe(false);
  });
});
