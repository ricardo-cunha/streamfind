import { ChromatogramsTableViewer } from '../../ChromatogramsTableViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.chromatogramsTable.chromatograms_table_viewer',
  viewerId: 'mass-spec.chromatograms-table.chromatograms-table-viewer',
  name: 'Raw chromatograms viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#chromatogramsTable',
  component: ChromatogramsTableViewer,
});
