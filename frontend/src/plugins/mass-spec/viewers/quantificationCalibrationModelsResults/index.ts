import { QuantifySamplesViewer } from '../../QuantifySamplesViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.quantificationCalibrationModelsTable.quantification_viewer',
  viewerId: 'mass-spec.quantification-calibration-models-table.quantification-viewer',
  name: 'Quantification calibration models viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#quantificationCalibrationModelsTable',
  component: QuantifySamplesViewer,
});
