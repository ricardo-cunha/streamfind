import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { FeatureInspector } from './FeatureInspector';
import './FeatureInspector.css';

export const massSpecFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.mass-spec',
    name: 'streamfind mass-spec viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['mass_spec'],
    artifactContracts: ['featuresTable'],
    capabilities: ['feature-inspector'],
  },
  setup(api) {
    api.registerViewer({
      id: 'mass-spec.feature-inspector',
      label: 'Mass-spec feature inspector',
      accepts: ['featuresTable'],
      component: FeatureInspector,
    });
  },
};

export default massSpecFrontendPlugin;
