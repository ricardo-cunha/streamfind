import { useEffect, useMemo, useState } from 'react';
import type { ResultDataResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { colorFor } from './featurePlotUtils';

type PeakRow = Record<string, string | null>;
type PlotTheme = { text: string; grid: string; background: string };

const EMPTY_PEAK_ROWS: PeakRow[] = [];
const PEAK_QUERY_LIMIT = 128;

function getPlotTheme(): PlotTheme {
  const readThemeColor = (name: string, fallback: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  return {
    text: readThemeColor('--sf-text', 'currentColor'),
    grid: readThemeColor('--sf-border', 'transparent'),
    background: readThemeColor('--sf-surface-raised', 'transparent'),
  };
}

function text(row: PeakRow, column: string): string {
  return row[column] ?? '';
}

function number(row: PeakRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function numericArray(row: PeakRow, column: string): number[] {
  const raw = row[column];
  if (!raw) return [];
  try {
    const value: unknown = JSON.parse(raw);
    if (!Array.isArray(value)) return [];
    return value.map(Number).filter(Number.isFinite);
  } catch {
    return [];
  }
}

function withAlpha(hex: string, alpha: number): string {
  const normalized = hex.replace('#', '');
  if (normalized.length !== 6) return hex;
  const red = Number.parseInt(normalized.slice(0, 2), 16);
  const green = Number.parseInt(normalized.slice(2, 4), 16);
  const blue = Number.parseInt(normalized.slice(4, 6), 16);
  if (![red, green, blue].every(Number.isFinite)) return hex;
  return `rgba(${red}, ${green}, ${blue}, ${alpha})`;
}

function peakLabel(row: PeakRow): string {
  const analysis = text(row, 'analysis') || '(unassigned analysis)';
  const name = text(row, 'name') || text(row, 'channel') || text(row, 'chromatogram_id') || '(unnamed trace)';
  const precursor = text(row, 'precursor_mz');
  const product = text(row, 'product_mz');
  const transition = precursor && product ? ` · Q1 ${precursor} → Q3 ${product}` : '';
  const peakId = text(row, 'peak_id');
  return `${analysis} · ${name}${transition}${peakId ? ` · peak ${peakId}` : ''}`;
}

function peakHoverTemplate(row: PeakRow): string {
  const metadata = [
    `Analysis: ${text(row, 'analysis') || '(unassigned)'}`,
    `Chromatogram: ${text(row, 'chromatogram_id') || '(unknown)'}`,
    `Precursor m/z: ${text(row, 'precursor_mz') || '(not available)'}`,
    `Product m/z: ${text(row, 'product_mz') || '(not available)'}`,
    `Peak RT: ${text(row, 'rt') || '(not available)'} s`,
    `RT window: ${text(row, 'rt_start') || '(not available)'}–${text(row, 'rt_end') || '(not available)'} s`,
    `Area: ${text(row, 'area') || '(not available)'}`,
    `Height: ${text(row, 'height') || '(not available)'}`,
  ].join('<br>');
  return `${metadata}<br>RT: %{x:.4f} s<br>Intensity: %{y:.4f}<extra>${peakLabel(row)}</extra>`;
}

function makePeakSpec(
  rows: PeakRow[],
  resultId: string,
  theme: PlotTheme,
  showRaw: boolean,
  showBaseline: boolean,
): VisualizationSpec {
  const traces = rows.flatMap((row) => {
    const rt = numericArray(row, 'eic_rt');
    const intensity = numericArray(row, 'eic_intensity');
    if (!rt.length || !intensity.length) return [];
    const pointCount = Math.min(rt.length, intensity.length);
    const x = rt.slice(0, pointCount);
    const y = intensity.slice(0, pointCount);
    const label = peakLabel(row);
    const color = colorFor(`${text(row, 'analysis')}\n${text(row, 'chromatogram_id')}`);
    const peakRt = number(row, 'rt');
    const peakHeight = number(row, 'height') || Math.max(...y);
    const result: PlotlyTrace[] = [
      {
        type: 'scattergl',
        mode: 'lines',
        name: label,
        legendgroup: label,
        line: { color, width: 1.8 },
        fill: 'tozeroy',
        fillcolor: withAlpha(color, 0.21),
        x,
        y,
        hovertemplate: peakHoverTemplate(row),
      },
      {
        type: 'scattergl',
        mode: 'markers',
        name: `${label} apex`,
        legendgroup: label,
        showlegend: false,
        marker: { color, size: 8, symbol: 'diamond' },
        x: [peakRt],
        y: [peakHeight],
        hovertemplate: peakHoverTemplate(row),
      },
    ];
    if (showRaw) {
      const raw = numericArray(row, 'eic_raw_intensity').slice(0, pointCount);
      if (raw.length === pointCount) {
        result.push({
          type: 'scattergl',
          mode: 'lines',
          name: `${label} raw`,
          legendgroup: label,
          showlegend: false,
          line: { color, width: 1, dash: 'dot' },
          opacity: 0.65,
          x,
          y: raw,
          hovertemplate: `${label}<br>Raw RT: %{x:.4f} s<br>Raw intensity: %{y:.4f}<extra></extra>`,
        });
      }
    }
    if (showBaseline) {
      const baseline = numericArray(row, 'eic_baseline').slice(0, pointCount);
      if (baseline.length === pointCount) {
        result.push({
          type: 'scattergl',
          mode: 'lines',
          name: `${label} baseline`,
          legendgroup: label,
          showlegend: false,
          line: { color, width: 1, dash: 'dash' },
          opacity: 0.75,
          x,
          y: baseline,
          hovertemplate: `${label}<br>Baseline RT: %{x:.4f} s<br>Baseline: %{y:.4f}<extra></extra>`,
        });
      }
    }
    return result;
  });
  const plottedPeaks = rows.filter(
    (row) => numericArray(row, 'eic_rt').length && numericArray(row, 'eic_intensity').length,
  );
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.chromatogram_peaks_viewer',
    semantic_type: 'mass_spec.chromatogram_peaks',
    title: 'Chromatogram peak windows',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 65, r: 20, t: 20, b: 50 },
        xaxis: {
          title: { text: 'Retention time (s)', font: { color: theme.text } },
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          zerolinecolor: theme.grid,
        },
        yaxis: {
          title: { text: 'Intensity', font: { color: theme.text } },
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          zerolinecolor: theme.grid,
        },
        hovermode: 'closest',
        showlegend: plottedPeaks.length > 1,
        legend: { bgcolor: theme.background, font: { color: theme.text } },
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        hoverlabel: { bgcolor: theme.background, font: { color: theme.text } },
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_result_ids: [resultId],
      producer_operation_id: 'frontend.mass_spec.chromatogram_peaks_viewer',
      producer_node_id: 'frontend.mass_spec.chromatogram_peaks_viewer',
    },
    fallback: { description: 'Chromatogram peak visualization unavailable.' },
  };
}

export function ChromatogramPeaksViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<{ resultId: string; response: ResultDataResponse } | null>(null);
  const [showRaw, setShowRaw] = useState(false);
  const [showBaseline, setShowBaseline] = useState(false);
  const [loadState, setLoadState] = useState<{ resultId: string; error?: string } | null>(null);
  const [, setThemeRevision] = useState(0);
  const resultId = artifact?.result_id ?? '';
  const loading = !loadState || loadState.resultId !== resultId;
  const error = loadState?.resultId === resultId ? loadState.error : undefined;
  const currentData = data?.resultId === resultId ? data.response : null;

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision((current) => current + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!artifact || !client) return;
    let cancelled = false;
    const loadPeakRows = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: artifact.result_id,
        mode: 'sample',
        columns: [
          'analysis',
          'chromatogram_id',
          'chromatogram_index',
          'peak_id',
          'channel',
          'polarity',
          'signal_type',
          'chromatogram_type',
          'precursor_mz',
          'product_mz',
          'rt',
          'rt_start',
          'rt_end',
          'height',
          'area',
          'eic_rt',
          'eic_intensity',
          'eic_raw_intensity',
          'eic_baseline',
        ],
        // Peak rows already contain bounded per-peak windows. Limit the
        // number of plotted peaks so a large result remains interactive.
        limit: PEAK_QUERY_LIMIT,
      });
      if (!cancelled) {
        setData({ resultId: artifact.result_id, response: { ...response, offset: 0, limit: response.rows.length } });
        setLoadState({ resultId: artifact.result_id });
      }
    };
    void loadPeakRows().catch((reason: unknown) => {
      if (!cancelled)
        setLoadState({
          resultId: artifact.result_id,
          error: reason instanceof Error ? reason.message : 'Unable to load chromatogram peak rows.',
        });
    });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId]);

  const rows = currentData?.rows ?? EMPTY_PEAK_ROWS;
  const theme = getPlotTheme();
  const spec = useMemo(
    () => (rows.length ? makePeakSpec(rows, resultId, theme, showRaw, showBaseline) : null),
    [resultId, rows, theme, showRaw, showBaseline],
  );

  if (!artifact || !client) return <div role="alert">Chromatogram peak result data is unavailable.</div>;
  if (loading) return <LoadingLogo label="Loading chromatogram peaks…" />;
  if (error)
    return (
      <div className="sf-chromatogram-peaks-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!rows.length || !spec)
    return <div className="sf-chromatogram-peaks-viewer-state">No chromatogram peak rows are available.</div>;

  return (
    <div className="sf-chromatogram-peaks-viewer">
      <div className="sf-chromatogram-peaks-viewer-toolbar" aria-label="Chromatogram peak trace controls">
        <span className="sf-chromatogram-peaks-viewer-toolbar-label">Peak windows</span>
        <label>
          <input type="checkbox" checked={showRaw} onChange={() => setShowRaw((current) => !current)} />
          raw signal
        </label>
        <label>
          <input type="checkbox" checked={showBaseline} onChange={() => setShowBaseline((current) => !current)} />
          baseline
        </label>
        <span className="sf-chromatogram-peaks-viewer-help">
          Filled traces are the integrated EIC windows; diamonds mark selected apices.
        </span>
        <span className="sf-chromatogram-peaks-viewer-summary">
          {currentData && currentData.total_rows > rows.length
            ? `showing ${rows.length} of ${currentData.total_rows} peaks · `
            : `${rows.length} rows · `}
          {spec.payload.data.length} traces
        </span>
      </div>
      <div className="sf-chromatogram-peaks-viewer-plot">
        <VisualizationRenderer spec={spec} />
      </div>
    </div>
  );
}
