import { FindFeaturesDebugViewer } from '../../FindFeaturesDebugViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.findFeaturesDebugResult.find_features_debug_viewer',
  viewerId: 'mass-spec.find-features-debug-result.find-features-debug-viewer',
  name: 'Find features diagnostics',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#findFeaturesDebugResult',
  component: FindFeaturesDebugViewer,
});
