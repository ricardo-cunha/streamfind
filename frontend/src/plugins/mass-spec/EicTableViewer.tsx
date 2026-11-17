import { useEffect, useMemo, useState } from 'react';
import type { ResultDataResponse, ResultQueryFilter } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { buildResultQueryFilter, formatFilterValue } from './massSpecFilterUtils';

type EicRow = Record<string, string | null>;

const SELECT_BY_COLUMNS = [
  ['analysis', 'analysis'],
  ['name', 'name'],
  ['replicate', 'replicate'],
  ['polarity', 'polarity'],
  ['level', 'level'],
] as const;
const FILTER_COLUMNS = [
  ['analysis', 'Analysis'],
  ['name', 'Name'],
  ['replicate', 'Replicate'],
  ['polarity', 'Polarity'],
  ['level', 'Level'],
] as const;
type SelectByColumn = (typeof SELECT_BY_COLUMNS)[number][0];
const DEFAULT_GROUP: SelectByColumn[] = ['analysis', 'name'];
const EMPTY_EIC_ROWS: EicRow[] = [];
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

export function EicTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<{ resultId: string; response: ResultDataResponse } | null>(null);
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
      const response = await client.resultQuery(context.sessionId, {
        result_id: artifact.result_id,
        mode: activeFilters.length ? 'page' : 'eic',
        columns: ['analysis', 'replicate', 'name', 'polarity', 'level', 'rt', 'intensity'],
        filters: filtersForQuery,
        // Keep the plotted EIC payload bounded while letting the service apply
        // scalar filters before it reads the trace data.
        limit: 65536,
        eic_max_points: 512,
      });
      if (!cancelled) {
        setData({ resultId: artifact.result_id, response: { ...response, offset: 0, limit: response.rows.length } });
        setLoadState({ key: queryKey });
      }
    };
    void loadRows().catch((reason: unknown) => {
      if (!cancelled)
        setLoadState({
          key: queryKey,
          error: reason instanceof Error ? reason.message : 'Unable to load EIC rows.',
        });
    });
    return () => {
      cancelled = true;
    };
  }, [activeFilters, artifact, client, context.sessionId, queryKey]);

  const rows = currentData?.rows ?? EMPTY_EIC_ROWS;
  const theme = getPlotTheme();
  const spec = useMemo(
    () => (rows.length ? makeSpec(rows, grouping, resultId, theme) : null),
    [resultId, grouping, rows, theme],
  );

  if (!artifact || !client) return <div role="alert">EIC result data is unavailable.</div>;
  if (loading && !currentData) return <LoadingLogo label="Loading EIC rows…" />;
  if (error)
    return (
      <div className="sf-eic-viewer-state" role="alert">
        {error}
      </div>
    );
  return (
    <div className="sf-eic-viewer">
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
            {rows.length.toLocaleString()} rows · {(spec?.payload.data.length ?? 0).toLocaleString()} traces
            {loading ? ' · loading…' : ''}
          </>
        }
      />
      {spec ? (
        <div className="sf-eic-viewer-plot">
          <VisualizationRenderer spec={spec} />
        </div>
      ) : (
        <div className="sf-eic-viewer-state">
          {activeFilters.length ? 'No EIC rows match the selected filters.' : 'No EIC rows are available.'}
        </div>
      )}
    </div>
  );
}
