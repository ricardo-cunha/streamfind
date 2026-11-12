import { useEffect, useMemo, useState } from 'react';
import type { ResultDataResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { colorFor } from './featurePlotUtils';

type EicRow = Record<string, string | null>;

const GROUP_COLUMNS = ['analysis', 'name', 'replicate', 'polarity', 'level'] as const;
const DEFAULT_GROUP = ['analysis', 'name'];
const EMPTY_EIC_ROWS: EicRow[] = [];
const PEAK_QUERY_LIMIT = 128;
type PlotTheme = { text: string; grid: string; background: string };

function getPlotTheme(): PlotTheme {
  const readThemeColor = (name: string, fallback: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  return {
    text: readThemeColor('--sf-text', 'currentColor'),
    grid: readThemeColor('--sf-border', 'transparent'),
    background: readThemeColor('--sf-surface-raised', 'transparent'),
  };
}

function text(row: EicRow, column: string): string {
  return row[column] ?? '';
}

function number(row: EicRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function numericArray(row: EicRow, column: string): number[] {
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

function contractName(contractId: string): string {
  return contractId.split('#').at(-1)?.split(':').at(-1) ?? contractId;
}

function peakLabel(row: EicRow): string {
  const analysis = text(row, 'analysis') || '(unassigned analysis)';
  const name = text(row, 'name') || text(row, 'channel') || text(row, 'chromatogram_id') || '(unnamed trace)';
  const precursor = text(row, 'precursor_mz');
  const product = text(row, 'product_mz');
  const transition = precursor && product ? ` · Q1 ${precursor} → Q3 ${product}` : '';
  const peakId = text(row, 'peak_id');
  return `${analysis} · ${name}${transition}${peakId ? ` · peak ${peakId}` : ''}`;
}

function peakHoverTemplate(row: EicRow): string {
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

function makeSpec(rows: EicRow[], colorGrouping: string[], resultId: string, theme: PlotTheme): VisualizationSpec {
  const groups = new Map<string, { rows: EicRow[]; colorKey: string; label: string }>();
  for (const row of rows) {
    const traceKey = [text(row, 'analysis'), text(row, 'name')].map((value) => value || '(empty)').join(' · ');
    const colorKey = colorGrouping.map((column) => text(row, column) || '(empty)').join(' · ');
    const group = groups.get(traceKey) ?? { rows: [], colorKey, label: traceKey };
    group.rows.push(row);
    groups.set(traceKey, group);
  }
  const colorKeys = [...new Set([...groups.values()].map((group) => group.colorKey))].sort();
  const colors = new Map(colorKeys.map((key, index) => [key, `hsl(${(index * 137.508) % 360} 65% 45%)`]));
  const legendGroups = new Set<string>();
  const traces = [...groups.values()].map((group) => {
    const sorted = [...group.rows].sort((left, right) => number(left, 'rt') - number(right, 'rt'));
    const color = colors.get(group.colorKey) ?? 'var(--sf-accent)';
    const showlegend = !legendGroups.has(group.colorKey);
    legendGroups.add(group.colorKey);
    return {
      type: 'scattergl',
      mode: 'lines',
      name: group.colorKey,
      showlegend,
      legendgroup: group.colorKey,
      line: { color },
      x: sorted.map((row) => number(row, 'rt')),
      y: sorted.map((row) => number(row, 'intensity')),
      hovertemplate: `${group.label}<br>Color group: ${group.colorKey}<br>RT: %{x}<br>Intensity: %{y}<extra></extra>`,
    };
  });
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.eic_table_viewer',
    semantic_type: 'mass_spec.eic',
    title: 'Extracted ion chromatograms',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 60, r: 20, t: 20, b: 50 },
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
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        hoverlabel: { bgcolor: theme.background, font: { color: theme.text } },
        legend: { bgcolor: theme.background, font: { color: theme.text } },
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_result_ids: [resultId],
      producer_operation_id: 'frontend.mass_spec.eic_table_viewer',
      producer_node_id: 'frontend.mass_spec.eic_table_viewer',
    },
    fallback: { description: 'EIC visualization unavailable.' },
  };
}

function makePeakSpec(
  rows: EicRow[],
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
        fillcolor: `${color}35`,
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

export function EicTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const isPeakTable = contractName(artifact?.contract_id ?? '') === 'chromatogramPeaksTable';
  const [data, setData] = useState<{ resultId: string; response: ResultDataResponse } | null>(null);
  const [grouping, setGrouping] = useState<string[]>(DEFAULT_GROUP);
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
    const loadAllRows = async () => {
      const columns = isPeakTable
        ? [
            'analysis',
            'chromatogram_id',
            'chromatogram_index',
            'peak_id',
            'name',
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
          ]
        : ['analysis', 'replicate', 'name', 'polarity', 'level', 'rt', 'intensity'];
      const response = await client.resultQuery(context.sessionId, {
        result_id: artifact.result_id,
        mode: isPeakTable ? 'sample' : 'eic',
        columns,
        // Peak rows already contain bounded per-peak windows. Limit the
        // number of plotted peaks so a large result remains interactive.
        limit: isPeakTable ? PEAK_QUERY_LIMIT : 65536,
        ...(isPeakTable ? {} : { eic_max_points: 512 }),
      });
      if (!cancelled) {
        setData({ resultId: artifact.result_id, response: { ...response, offset: 0, limit: response.rows.length } });
        setLoadState({ resultId: artifact.result_id });
      }
    };
    void loadAllRows().catch((reason: unknown) => {
      if (!cancelled)
        setLoadState({
          resultId: artifact.result_id,
          error: reason instanceof Error ? reason.message : `Unable to load ${isPeakTable ? 'peak' : 'EIC'} rows.`,
        });
    });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId, isPeakTable]);

  const rows = currentData?.rows ?? EMPTY_EIC_ROWS;
  const theme = getPlotTheme();
  const spec = useMemo(() => {
    if (!rows.length) return null;
    return isPeakTable
      ? makePeakSpec(rows, resultId, theme, showRaw, showBaseline)
      : makeSpec(rows, grouping, resultId, theme);
  }, [isPeakTable, resultId, grouping, rows, theme, showRaw, showBaseline]);

  if (!artifact || !client) return <div role="alert">EIC result data is unavailable.</div>;
  if (loading) return <LoadingLogo label={`Loading ${isPeakTable ? 'chromatogram peaks' : 'EIC rows'}…`} />;
  if (error)
    return (
      <div className="sf-eic-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!rows.length || !spec)
    return (
      <div className="sf-eic-viewer-state">No {isPeakTable ? 'chromatogram peak' : 'EIC'} rows are available.</div>
    );

  return (
    <div className="sf-eic-viewer">
      <div className="sf-eic-viewer-toolbar" aria-label={`${isPeakTable ? 'Chromatogram peak' : 'EIC'} trace controls`}>
        {isPeakTable ? (
          <>
            <span className="sf-eic-viewer-toolbar-label">Peak windows</span>
            <label>
              <input type="checkbox" checked={showRaw} onChange={() => setShowRaw((current) => !current)} />
              raw signal
            </label>
            <label>
              <input type="checkbox" checked={showBaseline} onChange={() => setShowBaseline((current) => !current)} />
              baseline
            </label>
            <span className="sf-eic-viewer-help">
              Filled traces are the integrated EIC windows; diamonds mark selected apices.
            </span>
          </>
        ) : (
          <>
            <span className="sf-eic-viewer-toolbar-label">Color/group traces by</span>
            {GROUP_COLUMNS.map((column) => (
              <label key={column}>
                <input
                  type="checkbox"
                  checked={grouping.includes(column)}
                  onChange={() =>
                    setGrouping((current) => {
                      if (current.includes(column)) {
                        const next = current.filter((value) => value !== column);
                        return next.length ? next : current;
                      }
                      return [...current, column];
                    })
                  }
                />
                {column}
              </label>
            ))}
          </>
        )}
        <span className="sf-eic-viewer-summary">
          {isPeakTable && currentData && currentData.total_rows > rows.length
            ? `showing ${rows.length} of ${currentData.total_rows} peaks · `
            : `${rows.length} rows · `}
          {spec.payload.data.length} traces
        </span>
      </div>
      <div className="sf-eic-viewer-plot">
        <VisualizationRenderer spec={spec} />
      </div>
    </div>
  );
}
