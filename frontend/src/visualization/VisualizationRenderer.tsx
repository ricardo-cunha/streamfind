import type { ReactNode } from 'react';
import { isVisualizationSpec, type VisualizationSpec } from './visualizationTypes';
import { visualizationRegistry, type VisualizationRendererProps } from './VisualizationRegistry';

export type VisualizationRendererViewProps = {
  spec: VisualizationSpec;
  className?: string;
  unsupported?: (spec: VisualizationSpec) => ReactNode;
};

export function VisualizationRenderer({ spec, className, unsupported }: VisualizationRendererViewProps): ReactNode {
  if (!isVisualizationSpec(spec)) {
    return <div role="alert">Invalid visualization specification.</div>;
  }
  const renderer = visualizationRegistry.resolve(spec.renderer.renderer_id);
  if (!renderer) {
    return (
      unsupported?.(spec) ?? (
        <div className={className} role="status">
          {spec.fallback.description}
        </div>
      )
    );
  }
  const props: VisualizationRendererProps = { spec, className };
  return renderer(props);
}
