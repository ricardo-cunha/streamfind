import { useEffect, useMemo, useRef, useState, type ReactElement } from 'react';
import type { PlotlyPayload, VisualizationSpec } from './visualizationTypes';
import type { VisualizationPointClick } from './VisualizationRegistry';

export type PlotlyRuntime = {
  newPlot: (
    element: HTMLElement,
    data: PlotlyPayload['data'],
    layout?: PlotlyPayload['layout'],
    config?: PlotlyPayload['config'],
  ) => void | Promise<unknown>;
  purge?: (element: HTMLElement) => void;
  resize?: (element: HTMLElement) => void | Promise<unknown>;
};

export type PlotlyRendererProps = {
  spec: VisualizationSpec;
  className?: string;
  onPointClick?: (point: VisualizationPointClick) => void;
  onPlotClick?: () => void;
  onDoubleClick?: () => void;
};

export function PlotlyRenderer({
  runtime,
  spec,
  className,
  onPointClick,
  onPlotClick,
  onDoubleClick,
}: PlotlyRendererProps & { runtime: PlotlyRuntime }): ReactElement {
  const chartRef = useRef<HTMLDivElement | null>(null);
  const pointClickRef = useRef(onPointClick);
  const plotClickRef = useRef(onPlotClick);
  const doubleClickRef = useRef(onDoubleClick);
  const [error, setError] = useState<string | null>(null);
  const renderKey = useMemo(() => JSON.stringify(spec.payload), [spec.payload]);
  useEffect(() => {
    pointClickRef.current = onPointClick;
    plotClickRef.current = onPlotClick;
    doubleClickRef.current = onDoubleClick;
  }, [onPointClick, onPlotClick, onDoubleClick]);

  // The serialized payload is the dependency by design: artifact refreshes recreate
  // the spec object, but must not reset Plotly's zoom/camera when its data is unchanged.
  /* eslint-disable react-hooks/exhaustive-deps */
  useEffect(() => {
    const element = chartRef.current;
    if (!element) return undefined;
    let active = true;
    const eventElement = element as HTMLDivElement & {
      on?: (event: string, handler: (event: { points?: VisualizationPointClick[] }) => void) => void;
      removeListener?: (event: string, handler: (event: { points?: VisualizationPointClick[] }) => void) => void;
    };
    const handlePointClick = (event: { points?: VisualizationPointClick[] }) => {
      const point = event.points?.[0];
      if (
        point &&
        (point.pointNumber !== undefined || point.pointIndex !== undefined || point.customdata !== undefined)
      )
        pointClickRef.current?.(point);
      else plotClickRef.current?.();
    };
    const handleDoubleClick = () => doubleClickRef.current?.();
    setError(null);
    const layout = { ...spec.payload.layout, autosize: true };
    Promise.resolve(runtime.newPlot(element, spec.payload.data, layout, spec.payload.config))
      .then(() => {
        eventElement.on?.('plotly_click', handlePointClick);
        eventElement.on?.('plotly_doubleclick', handleDoubleClick);
      })
      .catch((reason: unknown) => {
        if (active) setError(reason instanceof Error ? reason.message : 'Plotly failed to render the visualization');
      });
    return () => {
      active = false;
      eventElement.removeListener?.('plotly_click', handlePointClick);
      eventElement.removeListener?.('plotly_doubleclick', handleDoubleClick);
      runtime.purge?.(element);
    };
  }, [renderKey, runtime]);

  useEffect(() => {
    const element = chartRef.current;
    if (!element || typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(() => {
      void runtime.resize?.(element);
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, [runtime]);
  /* eslint-enable react-hooks/exhaustive-deps */

  return (
    <section className={className} aria-label={spec.title}>
      <div ref={chartRef} role="img" aria-label={spec.title} />
      {error ? (
        <p role="alert">
          {spec.fallback.description}: {error}
        </p>
      ) : null}
    </section>
  );
}
