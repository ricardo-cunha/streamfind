import { useEffect, useMemo, useState } from 'react';
import type { ResultDataResponse, ResultQueryFilter } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { colorFor } from './featurePlotUtils';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { buildResultQueryFilter, filterValueMatches, formatFilterValue } from './massSpecFilterUtils';

type PeakRow = Record<string, string | null>;
type PlotTheme = { text: string; grid: string; background: string };

const EMPTY_PEAK_ROWS: PeakRow[] = [];
const PEAK_QUERY_LIMIT = 4096;
const FILTERED_PEAK_QUERY_LIMIT = 250000;

const SELECT_BY_COLUMNS = [
  ['analysis', 'analysis'],
  ['precursor_mz', 'precursor m/z'],
  ['product_mz', 'product m/z'],
  ['chromatogram_type', 'type'],
  ['signal_type', 'signal'],
  ['detector', 'detector'],
  ['channel', 'channel'],
  ['wavelength_nm', 'wavelength'],
  ['polarity', 'polarity'],
  ['peak_id', 'peak'],
] as const;
const FILTER_COLUMNS = [
  ['analysis', 'Analysis'],
  ['precursor_mz', 'Precursor m/z'],
  ['product_mz', 'Product m/z'],
  ['wavelength_nm', 'Wavelength'],
  ['chromatogram_type', 'Chromatogram type'],
  ['signal_type', 'Signal type'],
  ['detector', 'Detector'],
  ['channel', 'Channel'],
  ['polarity', 'Polarity'],
] as const;
type SelectByColumn = (typeof SELECT_BY_COLUMNS)[number][0];
const DEFAULT_SELECT_BY: SelectByColumn[] = ['analysis', 'precursor_mz', 'product_mz'];

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

function shortText(value: string, maximum = 42): string {
  if (value.length <= maximum) return value;
  const side = Math.max(8, Math.floor((maximum - 1) / 2));
  return `${value.slice(0, side)}…${value.slice(-side)}`;
}

function compactLegendLabel(row: PeakRow, selectBy: SelectByColumn[]): string {
  const selected = new Set(selectBy);
  const labelParts: string[] = [];
  if (selected.has('analysis')) labelParts.push(shortText(text(row, 'analysis') || '(unassigned)'));
  if (selected.has('precursor_mz') && selected.has('product_mz')) {
    const precursor = text(row, 'precursor_mz');
    const product = text(row, 'product_mz');
    if (precursor || product) labelParts.push(`Q1 ${precursor || '?'} → Q3 ${product || '?'}`);
  } else {
    if (selected.has('precursor_mz')) labelParts.push(`Q1 ${text(row, 'precursor_mz') || '?'}`);
    if (selected.has('product_mz')) labelParts.push(`Q3 ${text(row, 'product_mz') || '?'}`);
  }
  for (const [column, name] of SELECT_BY_COLUMNS) {
    if (!selected.has(column) || column === 'analysis' || column === 'precursor_mz' || column === 'product_mz')
      continue;
    const value = text(row, column);
    if (value) labelParts.push(`${name} ${shortText(value, 24)}`);
  }
  return labelParts.join(' · ') || shortText(text(row, 'chromatogram_id') || 'trace');
}

function rowFilterKey(row: PeakRow, column: string): string {
  return text(row, column) || '(empty)';
}

function filterRows(rows: PeakRow[], filters: MassSpecFilterValues): PeakRow[] {
  return rows.filter((row) =>
    Object.entries(filters).every(
      ([column, values]) => !values.length || filterValueMatches(column, rowFilterKey(row, column), values),
    ),
  );
}

function escapeHoverText(value: string): string {
  return value.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function hoverNumber(row: PeakRow, column: string, digits = 3): string {
  const raw = text(row, column);
  if (!raw) return '(not available)';
  const value = Number(raw);
  if (!Number.isFinite(value)) return escapeHoverText(raw);
  if (value === 0) return '0';
  if (Math.abs(value) >= 10000 || Math.abs(value) < 0.001) return value.toExponential(3);
  return value.toLocaleString('en-US', { maximumFractionDigits: digits, useGrouping: false });
}

function peakHoverTemplate(row: PeakRow): string {
  const analysis = escapeHoverText(text(row, 'analysis') || '(unassigned)');
  const channelDetails = [
    text(row, 'chromatogram_type'),
    text(row, 'signal_type'),
    text(row, 'detector'),
    text(row, 'channel'),
  ]
    .filter(Boolean)
    .map(escapeHoverText)
    .join(' · ');
  const wavelength = text(row, 'wavelength_nm') ? `Wavelength: ${hoverNumber(row, 'wavelength_nm', 1)} nm` : '';
  const peakId = text(row, 'peak_id') ? `Peak: ${escapeHoverText(text(row, 'peak_id'))}` : '';
  const metadata = [
    `<b>${analysis}</b>`,
    `Q1 → Q3: ${hoverNumber(row, 'precursor_mz', 4)} → ${hoverNumber(row, 'product_mz', 4)}`,
    channelDetails ? `Channel: ${channelDetails}` : '',
    wavelength,
    peakId,
    `Peak RT: ${hoverNumber(row, 'rt', 2)} s · window ${hoverNumber(row, 'rt_start', 2)}–${hoverNumber(row, 'rt_end', 2)} s`,
    `Area: ${hoverNumber(row, 'area')} · Height: ${hoverNumber(row, 'height')}`,
    `Noise: ${hoverNumber(row, 'noise')} · S/N: ${hoverNumber(row, 'snr', 2)}`,
    `RT: %{x:.2f} s · Intensity: %{y:.4~f}`,
  ]
    .filter(Boolean)
    .join('<br>');
  // Keep the Plotly extra field empty: it otherwise renders the full SCIEX
  // chromatogram ID as a second, green hover line.
  return `${metadata}<extra></extra>`;
}

function makePeakSpec(
  rows: PeakRow[],
  resultId: string,
  theme: PlotTheme,
  selectBy: SelectByColumn[],
): VisualizationSpec {
  const legendGroups = new Set<string>();
  const traces = rows.flatMap((row) => {
    const rt = numericArray(row, 'eic_rt');
    const intensity = numericArray(row, 'eic_intensity');
    if (!rt.length || !intensity.length) return [];
    const pointCount = Math.min(rt.length, intensity.length);
    const x = rt.slice(0, pointCount);
    const y = intensity.slice(0, pointCount);
    const legendLabel = compactLegendLabel(row, selectBy);
    const showLegend = !legendGroups.has(legendLabel);
    legendGroups.add(legendLabel);
    const color = colorFor(`${text(row, 'analysis')}\n${text(row, 'chromatogram_id')}`);
    const peakRt = number(row, 'rt');
    const peakHeight = number(row, 'height') || Math.max(...y);
    const result: PlotlyTrace[] = [
      {
        type: 'scattergl',
        mode: 'lines',
        name: legendLabel,
        showlegend: showLegend,
        legendgroup: legendLabel,
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
        name: `${legendLabel} apex`,
        legendgroup: legendLabel,
        showlegend: false,
        marker: { color, size: 8, symbol: 'diamond' },
        x: [peakRt],
        y: [peakHeight],
        hovertemplate: peakHoverTemplate(row),
      },
    ];
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
  const [optionValues, setOptionValues] = useState<Record<string, string[]>>({});
  const [selectBy, setSelectBy] = useState<SelectByColumn[]>(DEFAULT_SELECT_BY);
  const [filters, setFilters] = useState<MassSpecFilterValues>({});
  const [loadState, setLoadState] = useState<{ key: string; error?: string } | null>(null);
  const [, setThemeRevision] = useState(0);
  const resultId = artifact?.result_id ?? '';
  const activeFilters = useMemo(() => Object.entries(filters).filter(([, values]) => values.length), [filters]);
  const queryKey = `${resultId}:${JSON.stringify(activeFilters)}`;
  const loading = !loadState || loadState.key !== queryKey;
  const error = loadState?.key === queryKey ? loadState.error : undefined;
  // Keep the previous plot mounted while a new multi-value filter request is
  // in flight so the dropdown can accept several checks without closing.
  const currentData = data?.resultId === resultId ? data.response : null;

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision((current) => current + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!artifact || !client) return;
    let cancelled = false;
    const loadDistinctOptions = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: artifact.result_id,
        mode: 'distinct',
        distinct_columns: FILTER_COLUMNS.map(([column]) => column),
      });
      if (cancelled) return;
      const values: Record<string, string[]> = {};
      for (const [column] of FILTER_COLUMNS) {
        const rawValues = response.distinct?.[column] ?? [];
        values[column] = [...new Set(rawValues.map((value) => formatFilterValue(column, value)))];
      }
      setOptionValues(values);
    };
    void loadDistinctOptions();
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId]);

  useEffect(() => {
    if (!artifact || !client) return;
    let cancelled = false;
    const filtersForQuery: ResultQueryFilter[] = activeFilters.map(([column, values]) =>
      buildResultQueryFilter(column, values),
    );
    const loadPeakRows = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: artifact.result_id,
        mode: activeFilters.length ? 'page' : 'sample',
        filters: filtersForQuery,
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
          'noise',
          'snr',
          'eic_rt',
          'eic_intensity',
        ],
        // Peak rows already contain bounded per-peak windows. Limit the
        // number of plotted peaks so a large result remains interactive.
        limit: activeFilters.length ? FILTERED_PEAK_QUERY_LIMIT : PEAK_QUERY_LIMIT,
      });
      if (!cancelled) {
        setData({ resultId: artifact.result_id, response: { ...response, offset: 0, limit: response.rows.length } });
        setLoadState({ key: queryKey });
      }
    };
    void loadPeakRows().catch((reason: unknown) => {
      if (!cancelled)
        setLoadState({
          key: queryKey,
          error: reason instanceof Error ? reason.message : 'Unable to load chromatogram peak rows.',
        });
    });
    return () => {
      cancelled = true;
    };
  }, [activeFilters, artifact, client, context.sessionId, queryKey]);

  const rows = currentData?.rows ?? EMPTY_PEAK_ROWS;
  const visibleRows = useMemo(() => filterRows(rows, filters), [filters, rows]);
  const theme = getPlotTheme();
  const spec = useMemo(
    () => (visibleRows.length ? makePeakSpec(visibleRows, resultId, theme, selectBy) : null),
    [resultId, selectBy, theme, visibleRows],
  );

  if (!artifact || !client) return <div role="alert">Chromatogram peak result data is unavailable.</div>;
  if (loading && !currentData) return <LoadingLogo label="Loading chromatogram peaks…" />;
  if (error)
    return (
      <div className="sf-chromatogram-peaks-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!rows.length)
    return <div className="sf-chromatogram-peaks-viewer-state">No chromatogram peak rows are available.</div>;

  return (
    <div className="sf-chromatogram-peaks-viewer">
      <MassSpecFilterToolbar
        selectByColumns={SELECT_BY_COLUMNS}
        filterColumns={FILTER_COLUMNS}
        selectBy={selectBy}
        onSelectByChange={(columns) => setSelectBy(columns as SelectByColumn[])}
        optionValues={optionValues}
        filters={filters}
        onFiltersChange={setFilters}
        summary={
          <>
            {currentData && currentData.total_rows > rows.length
              ? `showing ${rows.length} of ${currentData.total_rows} peaks`
              : `${rows.length} rows`}
            {visibleRows.length !== rows.length ? ` · ${visibleRows.length} selected` : ''}
            {spec ? ` · ${spec.payload.data.length} traces` : ''}
          </>
        }
      />
      {spec ? (
        <div className="sf-chromatogram-peaks-viewer-plot">
          <VisualizationRenderer spec={spec} />
        </div>
      ) : (
        <div className="sf-chromatogram-peaks-viewer-state">No peak rows match the selected filters.</div>
      )}
    </div>
  );
}
