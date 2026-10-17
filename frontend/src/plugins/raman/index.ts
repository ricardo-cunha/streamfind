import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { RamanSpectraViewer } from './RamanSpectraViewer';
import './RamanSpectraViewer.css';

export const ramanFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.raman',
    name: 'streamfind Raman viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['raman'],
    artifactContracts: ['spectraTable'],
    capabilities: ['raman-spectra-viewer'],
  },
  setup(api) {
    api.registerViewer({
      id: 'raman.spectra-viewer',
      label: 'Raman spectra viewer',
      accepts: ['spectraTable'],
      component: RamanSpectraViewer,
    });
  },
};

export default ramanFrontendPlugin;
