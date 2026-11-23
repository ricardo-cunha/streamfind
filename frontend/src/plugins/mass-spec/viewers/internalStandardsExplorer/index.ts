import { SuspectsExplorer } from '../../SuspectsExplorer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.internalStandardsTable.suspects_explorer',
  viewerId: 'mass-spec.internal-standards-table.suspects-explorer',
  name: 'Internal standards explorer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#internalStandardsTable',
  component: SuspectsExplorer,
});
