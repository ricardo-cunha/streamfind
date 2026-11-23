import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import type { ViewerComponent } from '../../framework/viewers/viewerTypes';

export function createMassSpecViewerPlugin(options: {
  id: string;
  viewerId: string;
  name: string;
  contract: string;
  component: ViewerComponent;
}): FrontendPlugin {
  return {
    manifest: {
      schemaVersion: 1,
      kind: 'viewer',
      id: options.id,
      name: options.name,
      version: '1.0.0',
      apiVersion: FRONTEND_PLUGIN_API_VERSION,
      plugins: ['mass_spec'],
      artifactContracts: [options.contract],
    },
    setup(api) {
      api.registerViewer({
        id: options.viewerId,
        label: options.name,
        accepts: [options.contract],
        component: options.component,
      });
    },
  };
}
