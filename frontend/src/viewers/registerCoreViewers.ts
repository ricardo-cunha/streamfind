import { FeatureInspector } from './FeatureInspector';
import { VisualizationArtifactViewer } from './VisualizationArtifactViewer';
import { viewerRegistry, type ViewerRegistry } from './viewerTypes';

export function registerCoreViewers(registry: ViewerRegistry = viewerRegistry): void {
  if (registry.has('core.visualization')) return;

  registry.register({
    id: 'mass-spec.feature-inspector',
    label: 'Mass-spec feature inspector',
    accepts: ['featuresTable'],
    component: FeatureInspector,
  });

  registry.register({
    id: 'core.visualization',
    label: 'StreamFind visualization',
    accepts: ['visualizationSpecResult'],
    component: VisualizationArtifactViewer,
  });
}
