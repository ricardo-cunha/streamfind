import { useEffect, useMemo, useState } from 'react';
import type { ResultQueryFilter, ResultQueryResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { colorFor } from './featurePlotUtils';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { buildResultQueryFilter, filterValueMatches, formatFilterValue } from './massSpecFilterUtils';

type ChromatogramRow = Record<string, unknown>;
type PlotTheme = { text: string; grid: string; background: string };
type SelectByColumn =
  | 'analysis'
  | 'precursor_mz'
  | 'product_mz'
  | 'chromatogram_type'
  | 'signal_type'
  | 'detector'
  | 'channel'
  | 'wavelength_nm'
  | 'polarity'
  | 'index';

const EMPTY_ROWS: ChromatogramRow[] = [];
const INITIAL_POINT_LIMIT = 65536;
// Keep filtered plot responses bounded to the same interactive point budget
// as the initial sample. Returning hundreds of thousands of JSON row objects
// can stall the browser before Plotly gets a chance to render the new filter.
const FILTERED_POINT_LIMIT = 65536;
const TABLE_COLUMNS = [
  'analysis',
  'index',
  'chromatogram_id',
  'polarity',
  'precursor_mz',
  'product_mz',
  'activation_ce',
  'signal_type',
  'chromatogram_type',
  'detector',
  'channel',
  'wavelength_nm',
  'units',
  'rt',
  'intensity',
];
const SELECT_BY_COLUMNS: Array<[SelectByColumn, string]> = [
  ['analysis', 'analysis'],
  ['precursor_mz', 'precursor m/z'],
  ['product_mz', 'product m/z'],
  ['chromatogram_type', 'type'],
  ['signal_type', 'signal'],
  ['detector', 'detector'],
  ['channel', 'channel'],
  ['wavelength_nm', 'wavelength'],
  ['polarity', 'polarity'],
  ['index', 'index'],
];
const FILTER_COLUMNS: Array<[SelectByColumn, string]> = [
  ['analysis', 'Analysis'],
  ['index', 'Chromatogram index'],
  ['precursor_mz', 'Precursor m/z'],
  ['product_mz', 'Product m/z'],
  ['wavelength_nm', 'Wavelength'],
  ['chromatogram_type', 'Chromatogram type'],
  ['signal_type', 'Signal type'],
  ['detector', 'Detector'],
  ['channel', 'Channel'],
  ['polarity', 'Polarity'],
];
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

function text(row: ChromatogramRow, column: string): string {
  const value = row[column];
  return value === null || value === undefined ? '' : String(value);
}

function number(row: ChromatogramRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function shortText(value: string, maximum = 42): string {
  if (value.length <= maximum) return value;
  const side = Math.max(8, Math.floor((maximum - 1) / 2));
  return `${value.slice(0, side)}…${value.slice(-side)}`;
}

function valueKey(row: ChromatogramRow, column: string): string {
  return text(row, column) || '(empty)';
}

function filterRows(rows: ChromatogramRow[], filters: MassSpecFilterValues): ChromatogramRow[] {
  return rows.filter((row) =>
    Object.entries(filters).every(([column, values]) => !values.length || filterValueMatches(column, text(row, column), values)),
  );
}

function compactLabel(row: ChromatogramRow, selectBy: SelectByColumn[]): string {
  const selected = new Set(selectBy);
  const parts: string[] = [];
  if (selected.has('analysis')) parts.push(shortText(text(row, 'analysis') || '(unassigned)'));
  if (selected.has('precursor_mz') && selected.has('product_mz')) {
    parts.push(`Q1 ${text(row, 'precursor_mz') || '?'} → Q3 ${text(row, 'product_mz') || '?'}`);
  } else {
    if (selected.has('precursor_mz')) parts.push(`Q1 ${text(row, 'precursor_mz') || '?'}`);
    if (selected.has('product_mz')) parts.push(`Q3 ${text(row, 'product_mz') || '?'}`);
  }
  for (const [column, label] of SELECT_BY_COLUMNS) {
    if (!selected.has(column) || column === 'analysis' || column === 'precursor_mz' || column === 'product_mz')
      continue;
    const value = text(row, column);
    if (value) parts.push(`${label} ${shortText(value, 24)}`);
  }
  return parts.join(' · ') || shortText(text(row, 'chromatogram_id') || 'trace');
}

function makeSpec(
  rows: ChromatogramRow[],
  resultId: string,
  theme: PlotTheme,
  selectBy: SelectByColumn[],
): VisualizationSpec {
  const groups = new Map<string, ChromatogramRow[]>();
  for (const row of rows) {
    const key = `${text(row, 'analysis')}\n${text(row, 'index')}`;
    const group = groups.get(key) ?? [];
    group.push(row);
    groups.set(key, group);
  }

  const legendGroups = new Set<string>();
  const traces: PlotlyTrace[] = [];
  for (const groupRows of groups.values()) {
    const sorted = [...groupRows].sort((left, right) => number(left, 'rt') - number(right, 'rt'));
    const first = sorted[0];
    const label = compactLabel(first, selectBy);
    const showLegend = !legendGroups.has(label);
    legendGroups.add(label);
    const color = colorFor(selectBy.map((column) => valueKey(first, column)).join('\n'));
    const x = sorted.map((row) => number(row, 'rt'));
    const intensity = sorted.map((row) => number(row, 'intensity'));
    const hover =
      `Analysis: ${text(first, 'analysis') || '(unassigned)'}<br>` +
      `Chromatogram: ${text(first, 'chromatogram_id') || '(unknown)'}<br>` +
      `Precursor m/z: ${text(first, 'precursor_mz') || '(not available)'}<br>` +
      `Product m/z: ${text(first, 'product_mz') || '(not available)'}<br>` +
      `RT: %{x:.4f} s<br>Intensity: %{y:.4f}<extra>${label}</extra>`;
    traces.push({
      type: 'scattergl',
      mode: 'lines',
      name: label,
      showlegend: showLegend,
      legendgroup: label,
      line: { color, width: 1.8 },
      x,
      y: intensity,
      hovertemplate: hover,
    });
  }

  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.chromatograms_table_viewer',
    semantic_type: 'mass_spec.chromatograms',
    title: 'Raw chromatograms',
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
        showlegend: traces.some((trace) => trace.showlegend !== false),
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
      producer_operation_id: 'frontend.mass_spec.chromatograms_table_viewer',
      producer_node_id: 'frontend.mass_spec.chromatograms_table_viewer',
    },
    fallback: { description: 'Chromatogram visualization unavailable.' },
  };
}

export function ChromatogramsTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const resultId = artifact?.result_id ?? '';
  const [data, setData] = useState<{
    resultId: string;
    queryKey: string;
    filters: MassSpecFilterValues;
    response: ResultQueryResponse<unknown>;
  } | null>(null);
  const [optionValues, setOptionValues] = useState<Record<string, string[]>>({});
  const [selectBy, setSelectBy] = useState<SelectByColumn[]>(DEFAULT_SELECT_BY);
  const [filters, setFilters] = useState<MassSpecFilterValues>({});
  const [loadState, setLoadState] = useState<{ key: string; error?: string } | null>(null);
  const [, setThemeRevision] = useState(0);
  const activeFilters = useMemo(() => Object.entries(filters).filter(([, values]) => values.length), [filters]);
  const queryKey = `${resultId}:${JSON.stringify(activeFilters)}`;
  const loading = !loadState || loadState.key !== queryKey;
  const error = loadState?.key === queryKey ? loadState.error : undefined;
  const dataMatchesQuery = data?.resultId === resultId && data.queryKey === queryKey;
  // Do not keep plotting the previous filter result while a new server query
  // is pending; otherwise the loading indicator sits over misleading traces.
  const currentData = dataMatchesQuery ? data.response : null;

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
    const loadRows = async () => {
      const response = await client.resultQuery(context.sessionId, {
        result_id: resultId,
        mode: activeFilters.length ? 'page' : 'sample',
        columns: TABLE_COLUMNS,
        filters: filtersForQuery,
        limit: activeFilters.length ? FILTERED_POINT_LIMIT : INITIAL_POINT_LIMIT,
        preserveComplexCells: true,
        compact_rows: true,
      });
      if (!cancelled) {
        setData({
          resultId,
          queryKey,
          filters: Object.fromEntries(activeFilters),
          response: { ...response, offset: 0, limit: response.rows.length },
        });
        setLoadState({ key: queryKey });
      }
    };
    const timer = window.setTimeout(() => {
      void loadRows().catch((reason: unknown) => {
        if (!cancelled)
          setLoadState({
            key: queryKey,
            error: reason instanceof Error ? reason.message : 'Unable to load chromatogram rows.',
          });
      });
    }, 120);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [activeFilters, client, context.sessionId, queryKey, resultId]);

  const rows = currentData?.rows ?? EMPTY_ROWS;
  const displayedFilters = dataMatchesQuery ? filters : data?.filters ?? filters;
  const visibleRows = useMemo(() => filterRows(rows, displayedFilters), [displayedFilters, rows]);
  const theme = getPlotTheme();
  const spec = useMemo(
    () => (visibleRows.length ? makeSpec(visibleRows, resultId, theme, selectBy) : null),
    [resultId, selectBy, theme, visibleRows],
  );

  if (!artifact || !client) return <div role="alert">Chromatogram result data is unavailable.</div>;
  if (loading) return <LoadingLogo label="Loading chromatograms…" />;
  return (
    <div className="sf-chromatograms-table-viewer">
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
            {visibleRows.length.toLocaleString()} displayed points
            {currentData && currentData.total_rows > visibleRows.length
              ? ` of ${currentData.total_rows.toLocaleString()}`
              : ''}{' '}
            · {spec?.payload.data.length ?? 0} traces
          </>
        }
      />
      {error ? (
        <div className="sf-chromatograms-table-viewer-state" role="alert">
          {error}
        </div>
      ) : spec ? (
        <div className="sf-chromatograms-table-viewer-plot">
          <VisualizationRenderer spec={spec} />
        </div>
      ) : (
        <div className="sf-chromatograms-table-viewer-state">
          {activeFilters.length ? 'No chromatogram rows match the selected filters.' : 'No chromatogram rows are available.'}
        </div>
      )}
    </div>
  );
}
