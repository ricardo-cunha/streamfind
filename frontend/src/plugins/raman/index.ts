import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { RamanSpectraViewer } from './RamanSpectraViewer';
import './RamanSpectraViewer.css';

export const ramanFrontendPlugin: FrontendPlugin = {
  manifest: {
    schemaVersion: 1,
    kind: 'viewer',
    id: 'native.viewer.raman',
    name: 'streamfind Raman viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    plugins: ['raman'],
    artifactContracts: ['https://streamfind.dev/catalogue/plugins/raman#spectraTable'],
    capabilities: ['raman-spectra-viewer'],
  },
  setup(api) {
    api.registerViewer({
      id: 'raman.spectra-viewer',
      label: 'Raman spectra viewer',
      accepts: ['https://streamfind.dev/catalogue/plugins/raman#spectraTable'],
      component: RamanSpectraViewer,
    });
  },
};

export default ramanFrontendPlugin;
