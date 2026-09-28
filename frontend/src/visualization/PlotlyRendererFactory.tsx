import type { VisualizationRenderer } from './VisualizationRegistry';
import { PlotlyRenderer, type PlotlyRuntime } from './PlotlyRenderer';

export function createPlotlyRenderer(runtime: PlotlyRuntime): VisualizationRenderer {
  return ({ spec, className }) => <PlotlyRenderer runtime={runtime} spec={spec} className={className} />;
}
