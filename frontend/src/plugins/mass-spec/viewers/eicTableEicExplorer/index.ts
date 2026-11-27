import { EicTableViewer } from '../../EicTableViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.eicTable.eic_table_explorer',
  viewerId: 'mass-spec.eic-table.eic-table-explorer',
  name: 'EIC viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#eicTable',
  component: EicTableViewer,
});
