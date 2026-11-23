import { TicTableViewer } from '../../TicTableViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.ticTable.tic_table_explorer',
  viewerId: 'mass-spec.tic-table.tic-table-explorer',
  name: 'TIC viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#ticTable',
  component: TicTableViewer,
});
