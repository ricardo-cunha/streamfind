import { frontendPluginRegistry, type FrontendPluginRegistry } from '../../framework/plugins/FrontendPluginRegistry';
import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { GenericTableViewer } from './artifact-viewers/GenericTableViewer';

export const coreFrontendPlugin: FrontendPlugin = {
  manifest: {
    schemaVersion: 1,
    kind: 'viewer',
    id: 'native.viewer.core',
    name: 'streamfind core viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    artifactContracts: ['https://streamfind.dev/catalogue/core#table'],
    artifactRepresentations: ['table'],
    capabilities: ['artifact-viewers'],
  },
  setup(api) {
    api.registerViewer({
      id: 'core.table',
      label: 'Table renderer',
      accepts: [],
      representations: ['table'],
      component: GenericTableViewer,
    });
  },
};

export function registerCoreFrontendPlugin(registry: FrontendPluginRegistry = frontendPluginRegistry): void {
  if (registry.has(coreFrontendPlugin.manifest.id)) return;
  registry.register(coreFrontendPlugin);
}
