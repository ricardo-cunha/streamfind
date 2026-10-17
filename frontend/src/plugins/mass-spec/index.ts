import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { FeatureInspector } from './FeatureInspector';
import './FeatureInspector.css';
import { SuspectTargetsViewer } from './SuspectTargetsViewer';
import './SuspectTargetsViewer.css';
import { SuspectsExplorer } from './SuspectsExplorer';
import './SuspectsExplorer.css';
import { EicTableViewer } from './EicTableViewer';
import './EicTableViewer.css';

export const massSpecFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.mass-spec',
    name: 'streamfind mass-spec viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['mass_spec'],
    artifactContracts: ['featuresTable', 'eicTable', 'suspectTargetsTable', 'suspectsTable', 'internalStandardsTable'],
    capabilities: ['feature-inspector', 'eic-viewer', 'suspect-targets-viewer', 'suspects-explorer'],
  },
  setup(api) {
    api.registerViewer({
      id: 'mass-spec.feature-inspector',
      label: 'Mass-spec feature inspector',
      accepts: ['featuresTable'],
      component: FeatureInspector,
    });
    api.registerViewer({
      id: 'mass-spec.suspect-targets-viewer',
      label: 'Suspect targets',
      accepts: ['suspectTargetsTable'],
      component: SuspectTargetsViewer,
    });
    api.registerViewer({
      id: 'mass-spec.eic-viewer',
      label: 'EIC viewer',
      accepts: ['eicTable'],
      component: EicTableViewer,
    });
    api.registerViewer({
      id: 'mass-spec.suspects-explorer',
      label: 'Suspects explorer',
      accepts: ['suspectsTable', 'internalStandardsTable'],
      component: SuspectsExplorer,
    });
  },
};

export default massSpecFrontendPlugin;
