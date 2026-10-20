import { useEffect, useMemo, useState, type PointerEvent as ReactPointerEvent, type ReactNode } from 'react';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import './FindFeaturesDebugViewer.css';

type DebugPayload = {
  enabled?: boolean;
  analysis?: string;
  mz?: number;
  spectrum_index?: number;
  log_text?: string;
};

type Point = { rt: number; mz: number; intensity: number };

const numberPattern = '[-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?';

export function FindFeaturesDebugViewer({ context }: ViewerComponentProps): ReactNode {
  const [loading, setLoading] = useState(true);
  const [leftPaneWidth, setLeftPaneWidth] = useState(300);
  const payload = useMemo(() => readPayload(context.artifact?.payload), [context.artifact?.payload]);
  const parsed = useMemo(() => (payload?.log_text ? parseLog(payload.log_text) : null), [payload?.log_text]);
  const spec = useMemo(() => (parsed ? makeSpec(parsed, payload ?? {}) : null), [parsed, payload]);

  const resizeLeftPane = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    const startX = event.clientX;
    const startWidth = leftPaneWidth;
    const move = (moveEvent: PointerEvent) => {
      setLeftPaneWidth(Math.max(220, Math.min(620, startWidth + moveEvent.clientX - startX)));
    };
    const stop = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', stop);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', stop, { once: true });
  };

  useEffect(() => {
    const timer = window.setTimeout(() => setLoading(false), 0);
    return () => window.clearTimeout(timer);
  }, []);

  if (loading) return <LoadingLogo label="Loading feature detection debug plot…" />;
  if (!payload?.enabled) {
    return <div className="sf-find-features-debug-empty">Debug selectors were not enabled for this run.</div>;
  }
  if (!parsed || !spec) {
    return (
      <section className="sf-find-features-debug-empty">
        <strong>The debug log was produced, but no plot data could be parsed.</strong>
        <pre>{payload.log_text ?? ''}</pre>
      </section>
    );
  }
  return (
    <main className="sf-find-features-debug-viewer" aria-label="Find features debug viewer">
      <div className="sf-find-features-debug-body" style={{ gridTemplateColumns: `${leftPaneWidth}px 1.25rem minmax(0, 1fr)` }}>
        <aside className="sf-find-features-debug-json" aria-label="Formatted debug JSON and diagnostic log">
          <h3>Debug JSON</h3>
          <pre>{JSON.stringify({ ...payload, log_text: undefined }, null, 2)}</pre>
          {payload.log_text ? (
            <div className="sf-find-features-debug-log">
              <h3>Diagnostic log</h3>
              <pre>{formattedLog(payload.log_text)}</pre>
            </div>
          ) : null}
        </aside>
        <div
          className="sf-resizable-splitter sf-find-features-debug-splitter"
          role="separator"
          aria-label="Resize debug JSON pane"
          aria-orientation="vertical"
          onPointerDown={resizeLeftPane}
        />
        <section className="sf-find-features-debug-plot">
          <VisualizationRenderer spec={spec} className="sf-find-features-debug-plotly" />
        </section>
      </div>
    </main>
  );
}

function readPayload(value: unknown): DebugPayload | null {
  if (typeof value === 'string') {
    try { return readPayload(JSON.parse(value)); } catch { return null; }
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  return value as DebugPayload;
}

function vector(text: string, name: string): number[] {
  const match = text.match(new RegExp(`${name}\\s*<-\\s*c\\(([^)]*)\\)`));
  if (!match) return [];
  return match[1].split(',').map((value) => Number(value.trim())).filter(Number.isFinite);
}

function clusterTable(text: string): { rt: number[]; mz: number[]; intensity: number[]; noise: number[]; smoothed: number[] } {
  const lines = text.split(/\r?\n/);
  const header = lines.findIndex((line) => line.trim() === 'cluster_rt\tcluster_mz\tcluster_intensity\tcluster_noise');
  const preparationHeader = lines.findIndex((line) => line.trim() === 'index\trt\tmz\tintensity\tbaseline\tsmoothed');
  const tableHeader = preparationHeader >= 0 ? preparationHeader : header;
  if (tableHeader < 0) return { rt: [], mz: [], intensity: [], noise: [], smoothed: [] };
  const rt: number[] = [];
  const mz: number[] = [];
  const intensity: number[] = [];
  const noise: number[] = [];
  const smoothed: number[] = [];
  for (const line of lines.slice(tableHeader + 1)) {
    const fields = line.trim().split(/\s+/).map(Number);
    const expectedFields = preparationHeader >= 0 ? 6 : 4;
    if (fields.length !== expectedFields || fields.some((value) => !Number.isFinite(value))) break;
    const offset = preparationHeader >= 0 ? 1 : 0;
    rt.push(fields[offset]);
    mz.push(fields[offset + 1]);
    intensity.push(fields[offset + 2]);
    noise.push(fields[offset + 3]);
    if (preparationHeader >= 0) smoothed.push(fields[offset + 4]);
  }
  return { rt, mz, intensity, noise, smoothed };
}

function formattedLog(text: string): string {
  return text.replace(/\\r\\n/g, '\n').replace(/\\n/g, '\n').replace(/\\t/g, '\t');
}

function pointsFromLog(text: string): Point[] {
  const points: Point[] = [];
  const expression = new RegExp(`(?:candidate index=\\d+|[-•✓×]\\s*)RT=(${numberPattern})[,\\s]+m\\/z=(${numberPattern})(?:[,\\s]+|\\s+)(?:smoothed(?:_intensity)?|intensity)=(${numberPattern})`, 'gi');
  for (const match of text.matchAll(expression)) {
    const rt = Number(match[1]);
    const mz = Number(match[2]);
    const intensity = Number(match[3]);
    if ([rt, mz, intensity].every(Number.isFinite)) points.push({ rt, mz, intensity });
  }
  return points;
}

function pointsForPattern(text: string, expression: RegExp): Point[] {
  const points: Point[] = [];
  for (const match of text.matchAll(expression)) {
    const point = { rt: Number(match[1]), mz: Number(match[2]), intensity: Number(match[3]) };
    if ([point.rt, point.mz, point.intensity].every(Number.isFinite)) points.push(point);
  }
  return points;
}

function parseLog(text: string) {
  const table = clusterTable(text);
  const clusterRt = table.rt.length ? table.rt : vector(text, 'cluster_rt');
  const clusterMz = table.mz.length ? table.mz : vector(text, 'cluster_mz');
  const clusterIntensity = table.intensity.length ? table.intensity : vector(text, 'cluster_intensity');
  const clusterSmoothed = table.smoothed.length ? table.smoothed : vector(text, 'cluster_smoothed');
  const clusterBaseline = table.noise.length ? table.noise : vector(text, 'cluster_baseline');
  const firstDerivative = vector(text, 'first_derivative');
  const secondDerivative = vector(text, 'second_derivative');
  const points = pointsFromLog(text);
  const candidates = points;
  const validPeaks: Point[] = [];
  for (const match of text.matchAll(new RegExp(`[✓+]\\s*ACCEPTED:\\s*RT=(${numberPattern}).*?intensity=(${numberPattern})`, 'gi'))) {
    const point = { rt: Number(match[1]), mz: Number.NaN, intensity: Number(match[2]) };
    if ([point.rt, point.intensity].every(Number.isFinite)) validPeaks.push(point);
  }
  const finalFeatures: Point[] = [];
  const finalFeatureExpression = new RegExp(`→\\s*FINAL FEATURE:[^\\n]*\\n\\s*RT=(${numberPattern})[^\\n]*\\n\\s*m\\/z=(${numberPattern})[^\\n]*\\n\\s*Intensity=(${numberPattern})`, 'g');
  for (const match of text.matchAll(finalFeatureExpression)) {
    const point = { rt: Number(match[1]), mz: Number(match[2]), intensity: Number(match[3]) };
    if ([point.rt, point.mz, point.intensity].every(Number.isFinite)) finalFeatures.push(point);
  }
  return { clusterRt, clusterMz, clusterIntensity, clusterSmoothed, clusterBaseline, firstDerivative, secondDerivative, candidates, points, validPeaks, finalFeatures };
}

function makeSpec(parsed: ReturnType<typeof parseLog>, payload: DebugPayload): VisualizationSpec {
  const readThemeColor = (name: string, fallback: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  const themeText = readThemeColor('--sf-text', '#f4fbfa');
  const themeBackground = readThemeColor('--sf-surface-raised', '#132226');
  const traces = [] as VisualizationSpec['payload']['data'];
  const length = Math.min(parsed.clusterRt.length, parsed.clusterIntensity.length);
  if (length) {
    traces.push({
      type: 'scattergl', mode: 'lines+markers', name: 'Raw intensity',
      x: parsed.clusterRt.slice(0, length), y: parsed.clusterIntensity.slice(0, length),
      line: { color: '#38bdf8', width: 1 }, marker: { color: '#38bdf8', size: 4 },
      customdata: parsed.clusterMz.slice(0, length), hovertemplate: 'RT=%{x}<br>Intensity=%{y}<br>m/z=%{customdata}<extra></extra>',
    });
  }
  if (parsed.clusterSmoothed.length) traces.push({ type: 'scattergl', mode: 'lines', name: 'Smoothed', x: parsed.clusterRt.slice(0, parsed.clusterSmoothed.length), y: parsed.clusterSmoothed, line: { color: '#f59e0b', width: 2 } });
  if (parsed.clusterBaseline.length) traces.push({ type: 'scattergl', mode: 'lines', name: 'Baseline / noise', x: parsed.clusterRt.slice(0, parsed.clusterBaseline.length), y: parsed.clusterBaseline, line: { color: '#a78bfa', dash: 'dot', width: 2 } });
  if (parsed.firstDerivative.length) traces.push({ type: 'scattergl', mode: 'lines', name: '1st derivative', x: parsed.clusterRt.slice(0, parsed.firstDerivative.length), y: parsed.firstDerivative, line: { color: '#f59e0b', width: 1.5 } });
  if (parsed.secondDerivative.length) traces.push({ type: 'scattergl', mode: 'lines', name: '2nd derivative', x: parsed.clusterRt.slice(0, parsed.secondDerivative.length), y: parsed.secondDerivative, line: { color: '#f472b6', width: 1.5, dash: 'dash' } });
  if (parsed.candidates.length) traces.push({ type: 'scatter', mode: 'markers', name: 'Candidates', x: parsed.candidates.map((point) => point.rt), y: parsed.candidates.map((point) => point.intensity), marker: { color: '#fb7185', size: 11, symbol: 'diamond' } });
  if (parsed.finalFeatures.length) traces.push({ type: 'scatter', mode: 'markers', name: 'Final features', x: parsed.finalFeatures.map((point) => point.rt), y: parsed.finalFeatures.map((point) => point.intensity), marker: { color: '#4ade80', size: 14, symbol: 'star', line: { color: '#ffffff', width: 1 } } });
  return {
    schema: 'streamfind.visualization/v1', visualization_id: 'mass_spec.find_features_debug', semantic_type: 'mass_spec.find_features_debug',
    title: 'Find features diagnostics', data_mode: 'inline', renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 65, r: 65, t: 20, b: 78 },
        paper_bgcolor: themeBackground,
        plot_bgcolor: themeBackground,
        hovermode: 'closest',
        font: { color: themeText },
        xaxis: { title: 'Retention time (s)' },
        yaxis: { title: 'Intensity' },
        legend: {
          bgcolor: themeBackground,
          font: { color: themeText },
          orientation: 'h',
          x: 0,
          xanchor: 'left',
          y: -0.12,
          yanchor: 'top',
        },
        uirevision: 'find-features-debug',
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: { source_artifact_ids: ['find-features-debug'], producer_operation_id: 'mass_spec.find_features', producer_node_id: 'mass_spec.find_features' },
    fallback: { description: `Feature debug data for ${payload.analysis || 'the selected analysis'} is unavailable.` },
  };
}
