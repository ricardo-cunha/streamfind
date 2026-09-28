import { useEffect, useMemo, useRef, useState, type ReactElement } from 'react';
import type { PlotlyPayload, VisualizationSpec } from './visualizationTypes';

export type PlotlyRuntime = {
  newPlot: (
    element: HTMLElement,
    data: PlotlyPayload['data'],
    layout?: PlotlyPayload['layout'],
    config?: PlotlyPayload['config'],
  ) => void | Promise<unknown>;
  purge?: (element: HTMLElement) => void;
};

export type PlotlyRendererProps = {
  spec: VisualizationSpec;
  className?: string;
};

export function PlotlyRenderer({
  runtime,
  spec,
  className,
}: PlotlyRendererProps & { runtime: PlotlyRuntime }): ReactElement {
  const chartRef = useRef<HTMLDivElement | null>(null);
  const [error, setError] = useState<string | null>(null);
  const renderKey = useMemo(() => JSON.stringify(spec.payload), [spec.payload]);

  // The serialized payload is the dependency by design: artifact refreshes recreate
  // the spec object, but must not reset Plotly's zoom/camera when its data is unchanged.
  /* eslint-disable react-hooks/exhaustive-deps */
  useEffect(() => {
    const element = chartRef.current;
    if (!element) return undefined;
    let active = true;
    setError(null);
    const layout = { ...spec.payload.layout, autosize: true };
    Promise.resolve(runtime.newPlot(element, spec.payload.data, layout, spec.payload.config)).catch(
      (reason: unknown) => {
        if (active) setError(reason instanceof Error ? reason.message : 'Plotly failed to render the visualization');
      },
    );
    return () => {
      active = false;
      runtime.purge?.(element);
    };
  }, [renderKey, runtime]);
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
