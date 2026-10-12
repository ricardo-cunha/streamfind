import type { VisualizationRenderer } from '../../../framework/visualization/VisualizationRegistry';
import { PlotlyRenderer, type PlotlyRuntime } from './PlotlyRenderer';

export function createPlotlyRenderer(runtime: PlotlyRuntime): VisualizationRenderer {
  return ({ spec, className, onPointClick, onPlotClick, onDoubleClick }) => (
    <PlotlyRenderer
      runtime={runtime}
      spec={spec}
      className={className}
      onPointClick={onPointClick}
      onPlotClick={onPlotClick}
      onDoubleClick={onDoubleClick}
    />
  );
}
