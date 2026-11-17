import { frontendPluginRegistry, type FrontendPluginRegistry } from '../../framework/plugins/FrontendPluginRegistry';
import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { GenericTableViewer } from './artifact-viewers/GenericTableViewer';

export const coreFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.core',
    name: 'streamfind core viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    artifactContracts: ['table'],
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
