import { useEffect, useRef, useState, type ReactElement } from 'react';
import type { PlotlyPayload, VisualizationSpec } from '../../../framework/visualization/visualizationTypes';
import type {
  VisualizationPointClick,
  VisualizationRelayout,
} from '../../../framework/visualization/VisualizationRegistry';

export type PlotlyRuntime = {
  newPlot: (
    element: HTMLElement,
    data: PlotlyPayload['data'],
    layout?: PlotlyPayload['layout'],
    config?: PlotlyPayload['config'],
  ) => void | Promise<unknown>;
  react?: (
    element: HTMLElement,
    data: PlotlyPayload['data'],
    layout?: PlotlyPayload['layout'],
    config?: PlotlyPayload['config'],
  ) => void | Promise<unknown>;
  purge?: (element: HTMLElement) => void;
  resize?: (element: HTMLElement) => void | Promise<unknown>;
};

type PlotlyLayout = Record<string, unknown>;

function readThemeColor(name: string, fallback: string): string {
  if (typeof document === 'undefined') return fallback;
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
}

function objectValue(value: unknown): PlotlyLayout {
  return value && typeof value === 'object' && !Array.isArray(value) ? (value as PlotlyLayout) : {};
}

/** Apply the application Plotly contract while preserving viewer-specific settings. */
function applyPlotlyTheme(layout: PlotlyLayout): PlotlyLayout {
  const text = readThemeColor('--sf-text', '#061314');
  const grid = readThemeColor('--sf-border', 'rgba(10, 48, 51, 0.14)');
  const background = readThemeColor('--sf-surface-raised', '#ffffff');
  const axis = {
    showgrid: true,
    gridcolor: grid,
    linecolor: text,
    tickcolor: text,
    tickfont: { color: text },
    titlefont: { color: text },
    zerolinecolor: grid,
  };
  return {
    ...layout,
    paper_bgcolor: layout.paper_bgcolor ?? background,
    plot_bgcolor: layout.plot_bgcolor ?? background,
    font: { color: text, ...objectValue(layout.font) },
    hoverlabel: { bgcolor: background, font: { color: text }, ...objectValue(layout.hoverlabel) },
    xaxis: { ...axis, ...objectValue(layout.xaxis) },
    yaxis: { ...axis, ...objectValue(layout.yaxis) },
  };
}

export type PlotlyRendererProps = {
  spec: VisualizationSpec;
  className?: string;
  onPointClick?: (point: VisualizationPointClick) => void;
  onPlotClick?: () => void;
  onDoubleClick?: () => void;
  onRelayout?: (event: VisualizationRelayout) => void;
};

export function PlotlyRenderer({
  runtime,
  spec,
  className,
  onPointClick,
  onPlotClick,
  onDoubleClick,
  onRelayout,
}: PlotlyRendererProps & { runtime: PlotlyRuntime }): ReactElement {
  const chartRef = useRef<HTMLDivElement | null>(null);
  const renderedElementRef = useRef<HTMLElement | null>(null);
  const initializedElementRef = useRef<HTMLElement | null>(null);
  const pointClickRef = useRef(onPointClick);
  const plotClickRef = useRef(onPlotClick);
  const doubleClickRef = useRef(onDoubleClick);
  const relayoutRef = useRef(onRelayout);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    pointClickRef.current = onPointClick;
    plotClickRef.current = onPlotClick;
    doubleClickRef.current = onDoubleClick;
    relayoutRef.current = onRelayout;
  }, [onPointClick, onPlotClick, onDoubleClick, onRelayout]);

  // The payload object identity is the dependency by design. Callers that update
  // data create a new payload; avoiding JSON serialization here is important for
  // large scatter plots because stringifying hundreds of thousands of points
  // blocks the browser before Plotly can render.
  /* eslint-disable react-hooks/exhaustive-deps */
  useEffect(() => {
    const element = chartRef.current;
    if (!element) return undefined;
    renderedElementRef.current = element;
    let active = true;
    const eventElement = element as HTMLDivElement & {
      on?: (
        event: string,
        handler: (event: VisualizationRelayout & { points?: VisualizationPointClick[] }) => void,
      ) => void;
      removeListener?: (
        event: string,
        handler: (event: VisualizationRelayout & { points?: VisualizationPointClick[] }) => void,
      ) => void;
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
    const handleRelayout = (event: VisualizationRelayout) => relayoutRef.current?.(event);
    let resizeFrame: number | undefined;
    let secondResizeFrame: number | undefined;
    const scheduleResize = () => {
      if (typeof requestAnimationFrame === 'undefined') {
        void runtime.resize?.(element);
        return;
      }
      resizeFrame = requestAnimationFrame(() => {
        void runtime.resize?.(element);
        // Plotly may create its canvas one frame after react() resolves,
        // especially when the modal has just become visible.
        secondResizeFrame = requestAnimationFrame(() => {
          void runtime.resize?.(element);
        });
      });
    };
    setError(null);
    // Keep Plotly's interaction state (zoom/pan and legend visibility) when
    // React supplies a new data/layout object after an interaction. Without a
    // stable uirevision, Plotly.react resets the axes to the autorange, which
    // is particularly noticeable for component EICs.
    const layout = {
      ...applyPlotlyTheme(spec.payload.layout ?? {}),
      autosize: true,
      uirevision: spec.visualization_id,
    };
    // Plotly.react is efficient for updates, but it is unreliable as the
    // first operation on a newly mounted/hidden modal. Create the graph once
    // with newPlot, then use react for subsequent payload updates.
    const render = initializedElementRef.current === element ? runtime.react ?? runtime.newPlot : runtime.newPlot;
    Promise.resolve(render(element, spec.payload.data, layout, spec.payload.config))
      .then(() => {
        if (!active) return;
        initializedElementRef.current = element;
        scheduleResize();
        eventElement.on?.('plotly_click', handlePointClick);
        eventElement.on?.('plotly_doubleclick', handleDoubleClick);
        eventElement.on?.('plotly_relayout', handleRelayout);
      })
      .catch((reason: unknown) => {
        if (active) setError(reason instanceof Error ? reason.message : 'Plotly failed to render the visualization');
      });
    return () => {
      active = false;
      eventElement.removeListener?.('plotly_click', handlePointClick);
      eventElement.removeListener?.('plotly_doubleclick', handleDoubleClick);
      eventElement.removeListener?.('plotly_relayout', handleRelayout);
      if (resizeFrame !== undefined) cancelAnimationFrame(resizeFrame);
      if (secondResizeFrame !== undefined) cancelAnimationFrame(secondResizeFrame);
    };
  }, [spec.payload, runtime]);

  useEffect(() => {
    return () => {
      if (renderedElementRef.current) runtime.purge?.(renderedElementRef.current);
      initializedElementRef.current = null;
    };
  }, [runtime]);

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
