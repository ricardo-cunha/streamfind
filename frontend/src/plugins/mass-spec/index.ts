import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { FeatureInspector } from './FeatureInspector';
import './FeatureInspector.css';
import { SuspectTargetsViewer } from './SuspectTargetsViewer';
import './SuspectTargetsViewer.css';
import { SuspectsExplorer } from './SuspectsExplorer';
import './SuspectsExplorer.css';
import { EicTableViewer } from './EicTableViewer';
import './EicTableViewer.css';
import { ChromatogramPeaksViewer } from './ChromatogramPeaksViewer';
import './ChromatogramPeaksViewer.css';
import { ChromatogramsTableViewer } from './ChromatogramsTableViewer';
import './ChromatogramsTableViewer.css';
import './MassSpecFilterToolbar.css';
import { FindFeaturesDebugViewer } from './FindFeaturesDebugViewer';
import { QuantifySamplesViewer } from './QuantifySamplesViewer';

export const massSpecFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.mass-spec',
    name: 'streamfind mass-spec viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['mass_spec'],
    artifactContracts: [
      'featuresTable',
      'eicTable',
      'chromatogramsTable',
      'chromatogramPeaksTable',
      'suspectTargetsTable',
      'suspectsTable',
      'internalStandardsTable',
      'findFeaturesDebugResult',
    'quantificationTable',
    'quantificationCalibrationModelsTable',
    ],
    capabilities: [
      'feature-inspector',
      'eic-viewer',
      'suspect-targets-viewer',
      'suspects-explorer',
      'find-features-debug-viewer',
      'quantification-viewer',
    ],
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
      id: 'mass-spec.chromatogram-peaks-viewer',
      label: 'Chromatogram peak viewer',
      accepts: ['chromatogramPeaksTable'],
      component: ChromatogramPeaksViewer,
    });
    api.registerViewer({
      id: 'mass-spec.chromatograms-table-viewer',
      label: 'Raw chromatograms viewer',
      accepts: ['chromatogramsTable'],
      component: ChromatogramsTableViewer,
    });
    api.registerViewer({
      id: 'mass-spec.suspects-explorer',
      label: 'Suspects explorer',
      accepts: ['suspectsTable', 'internalStandardsTable'],
      component: SuspectsExplorer,
    });
    api.registerViewer({
      id: 'mass-spec.find-features-debug-viewer',
      label: 'Find features diagnostics',
      accepts: ['findFeaturesDebugResult'],
      component: FindFeaturesDebugViewer,
    });
    api.registerViewer({
      id: 'mass-spec.quantification-viewer',
      label: 'Quantification curves and results',
      accepts: [
        'quantificationTable',
        'quantificationCalibrationModelsTable',
      ],
      component: QuantifySamplesViewer,
    });
  },
};

export default massSpecFrontendPlugin;
