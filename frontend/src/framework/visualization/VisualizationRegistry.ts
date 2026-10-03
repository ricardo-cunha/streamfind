import type { ReactNode } from 'react';
import type { VisualizationSpec } from './visualizationTypes';

export type VisualizationPointClick = { customdata?: unknown; pointIndex?: number; pointNumber?: number };

export type VisualizationRendererProps = {
  spec: VisualizationSpec;
  className?: string;
  onPointClick?: (point: VisualizationPointClick) => void;
  onPlotClick?: () => void;
  onDoubleClick?: () => void;
};

export type VisualizationRenderer = (props: VisualizationRendererProps) => ReactNode;

export class VisualizationRegistry {
  private readonly renderers = new Map<string, VisualizationRenderer>();

  register(rendererId: string, renderer: VisualizationRenderer): void {
    if (!rendererId.trim()) throw new Error('Visualization renderer id must not be empty');
    if (this.renderers.has(rendererId)) throw new Error(`Visualization renderer already registered: ${rendererId}`);
    this.renderers.set(rendererId, renderer);
  }

  resolve(rendererId: string): VisualizationRenderer | undefined {
    return this.renderers.get(rendererId);
  }

  has(rendererId: string): boolean {
    return this.renderers.has(rendererId);
  }

  unregister(rendererId: string): void {
    this.renderers.delete(rendererId);
  }

  ids(): string[] {
    return [...this.renderers.keys()].sort();
  }
}

export const visualizationRegistry = new VisualizationRegistry();
