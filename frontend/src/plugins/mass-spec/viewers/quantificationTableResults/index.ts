import { QuantifySamplesViewer } from '../../QuantifySamplesViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.quantificationTable.quantification_viewer',
  viewerId: 'mass-spec.quantification-table.quantification-viewer',
  name: 'Quantification results viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#quantificationTable',
  component: QuantifySamplesViewer,
});
