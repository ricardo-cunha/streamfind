import { useEffect, useMemo, useState } from 'react';
import type { ResultQueryFilter, ResultQueryResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { buildResultQueryFilter, formatFilterValue } from './massSpecFilterUtils';

type TicRow = Record<string, unknown>;
type PlotTheme = { text: string; grid: string; background: string };

const SELECT_BY_COLUMNS = [
  ['analysis', 'analysis'],
  ['replicate', 'replicate'],
  ['polarity', 'polarity'],
  ['level', 'level'],
] as const;
const FILTER_COLUMNS = [
  ['analysis', 'Analysis'],
  ['replicate', 'Replicate'],
  ['polarity', 'Polarity'],
  ['level', 'Level'],
] as const;
type SelectByColumn = (typeof SELECT_BY_COLUMNS)[number][0];
const DEFAULT_GROUP: SelectByColumn[] = ['analysis'];
const PAGE_SIZE = 65536;
const MAX_POINTS_PER_TRACE = 5000;
const EMPTY_ROWS: TicRow[] = [];

function getPlotTheme(): PlotTheme {
  const readThemeColor = (name: string, fallback: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  return {
    text: readThemeColor('--sf-text', 'currentColor'),
    grid: readThemeColor('--sf-border', 'transparent'),
    background: readThemeColor('--sf-surface-raised', 'transparent'),
  };
}

function value(row: TicRow, column: string): string {
  const cell = row[column];
  return cell === null || cell === undefined ? '(empty)' : String(cell);
}

function number(row: TicRow, column: string): number {
  const parsed = Number(row[column]);
  return Number.isFinite(parsed) ? parsed : 0;
}

function downsample(points: Array<{ rt: number; tic: number }>): Array<{ rt: number; tic: number }> {
  if (points.length <= MAX_POINTS_PER_TRACE) return points;
  const bucketCount = Math.floor(MAX_POINTS_PER_TRACE / 2);
  const bucketSize = points.length / bucketCount;
  const result: Array<{ rt: number; tic: number }> = [];
  for (let bucket = 0; bucket < bucketCount; bucket += 1) {
    const start = Math.floor(bucket * bucketSize);
    const end = Math.min(points.length, Math.floor((bucket + 1) * bucketSize));
    let minimum = points[start];
    let maximum = points[start];
    for (let index = start + 1; index < end; index += 1) {
      if (points[index].tic < minimum.tic) minimum = points[index];
      if (points[index].tic > maximum.tic) maximum = points[index];
    }
    if (minimum.rt <= maximum.rt) result.push(minimum, ...(maximum === minimum ? [] : [maximum]));
    else result.push(maximum, minimum);
  }
  return result;
}

function makeSpec(rows: TicRow[], grouping: string[], resultId: string, theme: PlotTheme): VisualizationSpec | null {
  const groups = new Map<string, Array<{ rt: number; tic: number }>>();
  for (const row of rows) {
    const key = grouping.map((column) => `${column}: ${value(row, column)}`).join(' · ') || 'All spectra';
    const points = groups.get(key) ?? [];
    points.push({ rt: number(row, 'rt'), tic: number(row, 'tic') });
    groups.set(key, points);
  }
  if (groups.size === 0) return null;
  const traces = [...groups.entries()]
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([label, values]) => {
      const points = downsample(values.sort((left, right) => left.rt - right.rt));
      return {
        type: 'scattergl',
        mode: 'lines',
        name: label,
        x: points.map((point) => point.rt),
        y: points.map((point) => point.tic),
        hovertemplate: `${label}<br>RT: %{x}<br>TIC: %{y}<extra></extra>`,
      };
    });
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.tic_table_viewer',
    semantic_type: 'mass_spec.spectra_tic',
    title: 'Total ion chromatograms',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 70, r: 20, t: 20, b: 50 },
        xaxis: {
          title: { text: 'Retention time (s)', font: { color: theme.text } },
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          zerolinecolor: theme.grid,
        },
        yaxis: {
          title: { text: 'Total ion current', font: { color: theme.text } },
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
      producer_operation_id: 'frontend.mass_spec.tic_table_viewer',
      producer_node_id: 'frontend.mass_spec.tic_table_viewer',
    },
    fallback: { description: 'TIC visualization unavailable.' },
  };
}

export function TicTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<{ resultId: string; response: ResultQueryResponse<unknown> } | null>(null);
  const [optionValues, setOptionValues] = useState<Record<string, string[]>>({});
  const [grouping, setGrouping] = useState<SelectByColumn[]>(DEFAULT_GROUP);
  const [filters, setFilters] = useState<MassSpecFilterValues>({});
  const [loadState, setLoadState] = useState<{ key: string; error?: string } | null>(null);
  const [, setThemeRevision] = useState(0);
  const resultId = artifact?.result_id ?? '';
  const activeFilters = useMemo(() => Object.entries(filters).filter(([, values]) => values.length), [filters]);
  const queryKey = `${resultId}:${JSON.stringify(activeFilters)}`;
  const loading = !loadState || loadState.key !== queryKey;
  const error = loadState?.key === queryKey ? loadState.error : undefined;
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
    void loadDistinctOptions().catch(() => {
      if (!cancelled) setOptionValues({});
    });
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
    const loadRows = async () => {
      const rows: TicRow[] = [];
      let offset = 0;
      let totalRows = Number.POSITIVE_INFINITY;
      while (offset < totalRows) {
        const response = await client.resultQuery(context.sessionId, {
          result_id: artifact.result_id,
          mode: 'page',
          columns: ['analysis', 'replicate', 'polarity', 'level', 'rt', 'tic'],
          filters: filtersForQuery,
          offset,
          limit: PAGE_SIZE,
          preserveComplexCells: true,
        });
        rows.push(...response.rows);
        offset += response.rows.length;
        totalRows = response.total_rows;
        if (!response.rows.length || !response.has_more) break;
      }
      if (!cancelled) {
        setData({
          resultId: artifact.result_id,
          response: {
            result_id: artifact.result_id,
            columns: [],
            rows,
            offset: 0,
            limit: rows.length,
            total_rows: Number.isFinite(totalRows) ? totalRows : rows.length,
            returned_rows: rows.length,
            mode: 'page',
            has_more: false,
          },
        });
        setLoadState({ key: queryKey });
      }
    };
    void loadRows().catch((reason: unknown) => {
      if (!cancelled)
        setLoadState({
          key: queryKey,
          error: reason instanceof Error ? reason.message : 'Unable to load TIC rows.',
        });
    });
    return () => {
      cancelled = true;
    };
  }, [activeFilters, artifact, client, context.sessionId, queryKey]);

  const rows = currentData?.rows ?? EMPTY_ROWS;
  const theme = getPlotTheme();
  const spec = useMemo(() => makeSpec(rows, grouping, resultId, theme), [grouping, resultId, rows, theme]);

  if (!artifact || !client) return <div role="alert">TIC result data is unavailable.</div>;
  if (loading && !currentData) return <LoadingLogo label="Loading TIC rows…" />;
  if (error)
    return (
      <div className="sf-tic-viewer-state" role="alert">
        {error}
      </div>
    );
  return (
    <div className="sf-tic-viewer">
      <MassSpecFilterToolbar
        selectByColumns={SELECT_BY_COLUMNS}
        filterColumns={FILTER_COLUMNS}
        selectBy={grouping}
        onSelectByChange={(columns) => setGrouping(columns as SelectByColumn[])}
        optionValues={optionValues}
        filters={filters}
        onFiltersChange={setFilters}
        summary={
          <>
            {rows.length.toLocaleString()} spectra · {(spec?.payload.data.length ?? 0).toLocaleString()} traces
            {loading ? ' · loading…' : ''}
          </>
        }
      />
      {spec ? (
        <div className="sf-tic-viewer-plot">
          <VisualizationRenderer spec={spec} />
        </div>
      ) : (
        <div className="sf-tic-viewer-state">
          {activeFilters.length ? 'No TIC rows match the selected filters.' : 'No TIC rows are available.'}
        </div>
      )}
    </div>
  );
}
