import type { PlotlyRuntime } from './PlotlyRenderer';
import { createPlotlyRenderer } from './PlotlyRendererFactory';
import { visualizationRegistry } from './VisualizationRegistry';

export function registerCoreVisualizationRenderers(runtime: PlotlyRuntime): void {
  if (!visualizationRegistry.has('core.plotly')) {
    visualizationRegistry.register('core.plotly', createPlotlyRenderer(runtime));
  }
}
