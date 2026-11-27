import { SuspectTargetsViewer } from '../../SuspectTargetsViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.suspectTargetsTable.suspect_targets_viewer',
  viewerId: 'mass-spec.suspect-targets-table.suspect-targets-viewer',
  name: 'Suspect targets',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#suspectTargetsTable',
  component: SuspectTargetsViewer,
});
