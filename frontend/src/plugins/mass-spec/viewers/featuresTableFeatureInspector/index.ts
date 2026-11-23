import { FeatureInspector } from '../../FeatureInspector';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.featuresTable.feature_inspector',
  viewerId: 'mass-spec.features-table.feature-inspector',
  name: 'Mass-spec feature inspector',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#featuresTable',
  component: FeatureInspector,
});
