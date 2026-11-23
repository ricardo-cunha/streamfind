import { SuspectsExplorer } from '../../SuspectsExplorer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.suspectsTable.suspects_explorer',
  viewerId: 'mass-spec.suspects-table.suspects-explorer',
  name: 'Suspects explorer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#suspectsTable',
  component: SuspectsExplorer,
});
