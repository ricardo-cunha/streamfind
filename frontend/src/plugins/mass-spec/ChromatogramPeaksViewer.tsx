import { useEffect, useMemo, useState } from 'react';
import type { ResultQueryFilter, ResultQueryResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { colorFor } from './featurePlotUtils';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { buildResultQueryFilter, filterValueMatches, formatFilterValue } from './massSpecFilterUtils';

type PeakRow = Record<string, unknown>;
type PlotTheme = { text: string; grid: string; background: string };

const EMPTY_PEAK_ROWS: PeakRow[] = [];
const PEAK_QUERY_LIMIT = 1024;
const FILTERED_PEAK_QUERY_LIMIT = 2048;
const MAX_POINTS_PER_PEAK = 128;

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
  const value = row[column];
  if (value === null || value === undefined) return '';
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'boolean') return String(value);
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function number(row: PeakRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function numericArray(row: PeakRow, column: string): number[] {
  const raw = row[column];
  if (Array.isArray(raw)) return raw.map((value) => (value === null ? Number.NaN : Number(value)));
  if (typeof raw !== 'string' || !raw.trim()) return [];
  try {
    let value: unknown = JSON.parse(raw);
    if (typeof value === 'string') value = JSON.parse(value);
    if (!Array.isArray(value)) return [];
    return value.map((entry) => (entry === null ? Number.NaN : Number(entry)));
  } catch {
    return [];
  }
}

function maxFinite(values: number[]): number {
  let maximum = Number.NEGATIVE_INFINITY;
  for (const value of values) if (Number.isFinite(value) && value > maximum) maximum = value;
  return maximum;
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

function peakHoverData(row: PeakRow): string[] {
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
  return [
    analysis,
    hoverNumber(row, 'precursor_mz', 4),
    hoverNumber(row, 'product_mz', 4),
    channelDetails,
    hoverNumber(row, 'wavelength_nm', 1),
    escapeHoverText(text(row, 'peak_id')),
    hoverNumber(row, 'rt', 2),
    hoverNumber(row, 'rt_start', 2),
    hoverNumber(row, 'rt_end', 2),
    hoverNumber(row, 'area'),
    hoverNumber(row, 'height'),
    hoverNumber(row, 'noise'),
    hoverNumber(row, 'snr', 2),
  ];
}

function peakHoverTemplate(): string {
  // Keep the Plotly extra field empty: it otherwise renders the full SCIEX
  // chromatogram ID as a second, green hover line.
  return [
    '<b>%{customdata[0]}</b>',
    'Q1 → Q3: %{customdata[1]} → %{customdata[2]}',
    'Channel: %{customdata[3]}',
    'Wavelength: %{customdata[4]} nm',
    'Peak: %{customdata[5]}',
    'Peak RT: %{customdata[6]} s · window %{customdata[7]}–%{customdata[8]} s',
    'Area: %{customdata[9]} · Height: %{customdata[10]}',
    'Noise: %{customdata[11]} · S/N: %{customdata[12]}',
    'RT: %{x:.2f} s · Intensity: %{y:.4~f}<extra></extra>',
  ].join('<br>');
}

function interpolateAt(x: number[], y: number[], target: number): number | null {
  for (let index = 0; index < x.length; index += 1) {
    if (x[index] === target) return y[index];
    if (index === 0 || x[index - 1] > target || target > x[index]) continue;
    const leftX = x[index - 1];
    const rightX = x[index];
    const leftY = y[index - 1];
    const rightY = y[index];
    if (rightX === leftX) return leftY;
    const fraction = (target - leftX) / (rightX - leftX);
    return leftY + fraction * (rightY - leftY);
  }
  return null;
}

function boundedPeakTrace(row: PeakRow, x: number[], y: number[]): { x: number[]; y: number[] } | null {
  const start = Number(row.rt_start);
  const end = Number(row.rt_end);
  if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return null;
  const startY = interpolateAt(x, y, start);
  const endY = interpolateAt(x, y, end);
  if (startY === null || endY === null) return null;
  const windowX = [start];
  const windowY = [startY];
  for (let index = 0; index < x.length; index += 1) {
    if (x[index] > start && x[index] < end) {
      windowX.push(x[index]);
      windowY.push(y[index]);
    }
  }
  windowX.push(end);
  windowY.push(endY);
  return { x: windowX, y: windowY };
}

function downsamplePeakTrace(x: number[], y: number[], maximumPoints: number): { x: number[]; y: number[] } {
  if (x.length <= maximumPoints) return { x, y };
  const bucketCount = Math.max(1, Math.floor((maximumPoints - 2) / 2));
  const bucketWidth = (x.length - 2) / bucketCount;
  const sampledX = [x[0]];
  const sampledY = [y[0]];
  for (let bucket = 0; bucket < bucketCount; bucket += 1) {
    const start = 1 + Math.floor(bucket * bucketWidth);
    const end = Math.min(x.length - 1, 1 + Math.floor((bucket + 1) * bucketWidth));
    let minIndex = start;
    let maxIndex = start;
    for (let index = start + 1; index < end; index += 1) {
      if (y[index] < y[minIndex]) minIndex = index;
      if (y[index] > y[maxIndex]) maxIndex = index;
    }
    for (const index of minIndex <= maxIndex ? [minIndex, maxIndex] : [maxIndex, minIndex]) {
      if (sampledX[sampledX.length - 1] !== x[index] || sampledY[sampledY.length - 1] !== y[index]) {
        sampledX.push(x[index]);
        sampledY.push(y[index]);
      }
    }
  }
  sampledX.push(x[x.length - 1]);
  sampledY.push(y[y.length - 1]);
  return { x: sampledX, y: sampledY };
}

type ParsedPeak = {
  row: PeakRow;
  x: number[];
  y: number[];
  integrated: { x: number[]; y: number[] } | null;
};

function parsePeakRows(rows: PeakRow[]): ParsedPeak[] {
  const parsed: ParsedPeak[] = [];
  for (const row of rows) {
    const rawRt = numericArray(row, 'eic_rt');
    const rawIntensity = numericArray(row, 'eic_intensity');
    const pointCount = Math.min(rawRt.length, rawIntensity.length);
    const fullX: number[] = [];
    const fullY: number[] = [];
    for (let index = 0; index < pointCount; index += 1) {
      if (!Number.isFinite(rawRt[index]) || !Number.isFinite(rawIntensity[index])) continue;
      fullX.push(rawRt[index]);
      fullY.push(rawIntensity[index]);
    }
    if (!fullX.length) continue;
    const integratedFull = boundedPeakTrace(row, fullX, fullY);
    const trace = downsamplePeakTrace(fullX, fullY, MAX_POINTS_PER_PEAK);
    parsed.push({
      row,
      x: trace.x,
      y: trace.y,
      integrated: integratedFull
        ? downsamplePeakTrace(integratedFull.x, integratedFull.y, MAX_POINTS_PER_PEAK)
        : null,
    });
  }
  return parsed;
}

type PeakPlotGroup = {
  row: PeakRow;
  label: string;
  color: string;
  lineX: Array<number | null>;
  lineY: Array<number | null>;
  lineSnr: Array<number | null>;
  fillX: Array<number | null>;
  fillY: Array<number | null>;
  apexX: number[];
  apexY: number[];
  apexData: string[][];
};

function makePeakSpec(
  peaks: ParsedPeak[],
  resultId: string,
  theme: PlotTheme,
  selectBy: SelectByColumn[],
): VisualizationSpec {
  const groups = new Map<string, PeakPlotGroup>();
  const legendGroups = new Set<string>();
  const orderedPeaks = [...peaks].sort((left, right) => number(left.row, 'rt') - number(right.row, 'rt'));
  for (const peak of orderedPeaks) {
    const { row, x, y, integrated } = peak;
    const legendLabel = compactLegendLabel(row, selectBy);
    const chromatogramKey = [
      text(row, 'analysis'),
      text(row, 'chromatogram_index') || text(row, 'index') || text(row, 'chromatogram_id'),
      legendLabel,
    ].join('\n');
    let group = groups.get(chromatogramKey);
    if (!group) {
      group = {
        row,
        label: legendLabel,
        color: colorFor(`${text(row, 'analysis')}\n${text(row, 'chromatogram_id')}`),
        lineX: [],
        lineY: [],
        lineSnr: [],
        fillX: [],
        fillY: [],
        apexX: [],
        apexY: [],
        apexData: [],
      };
      groups.set(chromatogramKey, group);
    }
    if (group.lineX.length) {
      group.lineX.push(null);
      group.lineY.push(null);
      group.lineSnr.push(null);
    }
    group.lineX.push(...x);
    group.lineY.push(...y);
    const snr = Number(text(row, 'snr'));
    for (let index = 0; index < x.length; index += 1) group.lineSnr.push(Number.isFinite(snr) ? snr : null);
    if (integrated) {
      if (group.fillX.length) {
        group.fillX.push(null);
        group.fillY.push(null);
      }
      group.fillX.push(...integrated.x);
      group.fillY.push(...integrated.y);
    }
    group.apexX.push(number(row, 'rt'));
    group.apexY.push(number(row, 'height') || maxFinite(y));
    group.apexData.push(peakHoverData(row));
  }

  const traces: PlotlyTrace[] = [];
  for (const group of groups.values()) {
    const showLegend = !legendGroups.has(group.label);
    legendGroups.add(group.label);
    traces.push({
      type: 'scattergl',
      mode: 'lines',
      name: group.label,
      showlegend: showLegend,
      legendgroup: group.label,
      line: { color: group.color, width: 1.8 },
      x: group.lineX,
      y: group.lineY,
      customdata: group.lineSnr,
      hovertemplate:
        `<b>${escapeHoverText(text(group.row, 'analysis') || '(unassigned)')}</b><br>` +
        `Q1 → Q3: ${hoverNumber(group.row, 'precursor_mz', 4)} → ${hoverNumber(group.row, 'product_mz', 4)}<br>` +
        `Wavelength: ${hoverNumber(group.row, 'wavelength_nm', 1)} nm<br>` +
        'S/N: %{customdata:.2f}<br>RT: %{x:.2f} s · Intensity: %{y:.4~f}<extra></extra>',
    });
    if (group.fillX.length) {
      traces.push({
        type: 'scattergl',
        mode: 'lines',
        name: `${group.label} integrated area`,
        showlegend: false,
        legendgroup: group.label,
        line: { color: group.color, width: 0 },
        fill: 'tozeroy',
        fillcolor: withAlpha(group.color, 0.21),
        x: group.fillX,
        y: group.fillY,
        hovertemplate: '<extra></extra>',
      });
    }
    traces.push({
      type: 'scattergl',
      mode: 'markers',
      name: `${group.label} apex`,
      legendgroup: group.label,
      showlegend: false,
      marker: { color: group.color, size: 8, symbol: 'diamond' },
      x: group.apexX,
      y: group.apexY,
      customdata: group.apexData,
      hovertemplate: peakHoverTemplate(),
    });
  }
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
        showlegend: peaks.length > 1,
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
  const [data, setData] = useState<{
    resultId: string;
    queryKey: string;
    filters: MassSpecFilterValues;
    response: ResultQueryResponse<unknown>;
  } | null>(null);
  const [optionValues, setOptionValues] = useState<Record<string, string[]>>({});
  const [selectBy, setSelectBy] = useState<SelectByColumn[]>(DEFAULT_SELECT_BY);
  const [filters, setFilters] = useState<MassSpecFilterValues>({});
  const [pageOffset, setPageOffset] = useState(0);
  const [loadState, setLoadState] = useState<{ key: string; error?: string } | null>(null);
  const [themeRevision, setThemeRevision] = useState(0);
  const resultId = artifact?.result_id ?? '';
  const activeFilters = useMemo(() => Object.entries(filters).filter(([, values]) => values.length), [filters]);
  const queryKey = `${resultId}:${JSON.stringify(activeFilters)}:${pageOffset}`;
  const loading = !loadState || loadState.key !== queryKey;
  const error = loadState?.key === queryKey ? loadState.error : undefined;
  // Keep the previous plot mounted while a new multi-value filter request is
  // in flight so the dropdown can accept several checks without closing.
  const currentData = data?.resultId === resultId ? data.response : null;
  const dataMatchesQuery = data?.resultId === resultId && data.queryKey === queryKey;

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision((current) => current + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!resultId || !client) return;
    let cancelled = false;
    const loadDistinctOptions = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: resultId,
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
    void loadDistinctOptions().catch(() => {
      if (!cancelled) setOptionValues({});
    });
    return () => {
      cancelled = true;
    };
  }, [client, context.sessionId, resultId]);

  useEffect(() => {
    if (!resultId || !client) return;
    let cancelled = false;
    const filtersForQuery: ResultQueryFilter[] = activeFilters.map(([column, values]) =>
      buildResultQueryFilter(column, values),
    );
    const loadPeakRows = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: resultId,
        mode: activeFilters.length || pageOffset > 0 ? 'page' : 'sample',
        offset: activeFilters.length ? pageOffset : 0,
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
        preserveComplexCells: true,
      });
      if (!cancelled) {
        setData({
          resultId,
          queryKey,
          filters: Object.fromEntries(activeFilters),
          response: { ...response, limit: response.rows.length },
        });
        setLoadState({ key: queryKey });
      }
    };
    const timer = window.setTimeout(() => {
      void loadPeakRows().catch((reason: unknown) => {
        if (!cancelled)
          setLoadState({
            key: queryKey,
            error: reason instanceof Error ? reason.message : 'Unable to load chromatogram peak rows.',
          });
      });
    }, 120);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [activeFilters, client, context.sessionId, pageOffset, queryKey, resultId]);

  const rows = currentData?.rows ?? EMPTY_PEAK_ROWS;
  const displayedFilters = dataMatchesQuery ? filters : data?.filters ?? filters;
  const visibleRows = useMemo(() => filterRows(rows, displayedFilters), [displayedFilters, rows]);
  const parsedPeaks = useMemo(() => parsePeakRows(visibleRows), [visibleRows]);
  const theme = useMemo(() => getPlotTheme(), [themeRevision]);
  const spec = useMemo(
    () => (parsedPeaks.length ? makePeakSpec(parsedPeaks, resultId, theme, selectBy) : null),
    [parsedPeaks, resultId, selectBy, theme],
  );
  const hasPlotData = Boolean(spec?.payload.data.length);

  if (!artifact || !client) return <div role="alert">Chromatogram peak result data is unavailable.</div>;
  if (loading && !currentData) return <LoadingLogo label="Loading chromatogram peaks…" />;
  return (
    <div className="sf-chromatogram-peaks-viewer">
      <MassSpecFilterToolbar
        selectByColumns={SELECT_BY_COLUMNS}
        filterColumns={FILTER_COLUMNS}
        selectBy={selectBy}
        onSelectByChange={(columns) => setSelectBy(columns as SelectByColumn[])}
        optionValues={optionValues}
        filters={filters}
        onFiltersChange={(nextFilters) => {
          setFilters(nextFilters);
          setPageOffset(0);
        }}
        summary={
          <>
            {currentData && currentData.total_rows > rows.length
              ? activeFilters.length
                ? `showing ${(currentData.offset ?? 0) + 1}–${(currentData.offset ?? 0) + rows.length} of ${currentData.total_rows} peaks`
                : `showing ${rows.length} of ${currentData.total_rows} peaks`
              : `${rows.length} rows`}
            {visibleRows.length !== rows.length ? ` · ${visibleRows.length} selected` : ''}
            {spec ? ` · ${spec.payload.data.length} traces` : ''}
            {currentData && currentData.total_rows > rows.length ? (
              <>
                {' '}
                <button
                  type="button"
                  disabled={(currentData.offset ?? 0) === 0}
                  onClick={() => setPageOffset(Math.max(0, (currentData.offset ?? 0) - FILTERED_PEAK_QUERY_LIMIT))}
                >
                  Previous peaks
                </button>{' '}
                <button
                  type="button"
                  disabled={!currentData.has_more}
                  onClick={() => setPageOffset((currentData.offset ?? 0) + rows.length)}
                >
                  Next peaks
                </button>
              </>
            ) : null}
          </>
        }
      />
      <div className="sf-chromatogram-peaks-viewer-content">
        {error ? (
          <div className="sf-chromatogram-peaks-viewer-state" role="alert">
            {error}
          </div>
        ) : spec && hasPlotData ? (
          <div className="sf-chromatogram-peaks-viewer-plot">
            <VisualizationRenderer spec={spec} />
          </div>
        ) : loading ? null : (
          <div className="sf-chromatogram-peaks-viewer-state">
            {!rows.length
              ? activeFilters.length
                ? 'No chromatogram peak rows match the selected filters.'
                : 'No chromatogram peak rows are available.'
              : 'The selected peak rows do not contain parseable EIC point arrays.'}
          </div>
        )}
        {loading ? (
          <div className="sf-chromatogram-peaks-viewer-loading">
            <LoadingLogo label="Loading chromatogram peaks…" />
          </div>
        ) : null}
      </div>
    </div>
  );
}
