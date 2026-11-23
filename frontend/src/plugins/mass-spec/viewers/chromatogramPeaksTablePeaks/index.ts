import { ChromatogramPeaksViewer } from '../../ChromatogramPeaksViewer';
import '../../massSpecViewerStyles';
import { createMassSpecViewerPlugin } from '../../massSpecViewerFactory';

export default createMassSpecViewerPlugin({
  id: 'native.viewer.mass_spec.chromatogramPeaksTable.chromatogram_peaks_viewer',
  viewerId: 'mass-spec.chromatogram-peaks-table.chromatogram-peaks-viewer',
  name: 'Chromatogram peak viewer',
  contract: 'https://streamfind.dev/catalogue/plugins/mass_spec#chromatogramPeaksTable',
  component: ChromatogramPeaksViewer,
});
