import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { ArtifactDataResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import type { VisualizationRelayout } from '../../framework/visualization/VisualizationRegistry';

type RamanRow = Record<string, string | null>;

const GROUP_COLUMNS = ['analysis_id', 'source_frame_index', 'time_seconds', 'axis_kind', 'signal_kind'] as const;
const DEFAULT_GROUP = ['analysis_id'];
const INITIAL_MAX_POINTS_PER_TRACE = 1024;
const INITIAL_MAX_ROWS = 500000;
const VIEWPORT_MAX_POINTS_PER_TRACE = 2000;
const VIEWPORT_MAX_ROWS = 1000000;

function text(row: RamanRow, column: string): string {
  return row[column] ?? '';
}

function number(row: RamanRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function labelPart(row: RamanRow, column: string, fallback: string): string {
  return text(row, column) || fallback;
}

function spectrumLabel(row: RamanRow): string {
  const analysis = labelPart(row, 'analysis_id', '(analysis)');
  const frame = labelPart(row, 'source_frame_index', 'frame ?');
  const time = labelPart(row, 'time_seconds', labelPart(row, 'frame_time_value', 'time ?'));
  const spectrum = labelPart(row, 'spectrum_id', `spectrum ${labelPart(row, 'spectrum_index', '?')}`);
  return `${analysis} · ${spectrum} · ${frame} · time ${time}`;
}

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

function makeSpec(
  rows: RamanRow[],
  colorGrouping: string[],
  artifactId: string,
  theme: PlotTheme,
  viewportRange: [number, number] | null,
): VisualizationSpec {
  const groups = new Map<string, { rows: RamanRow[]; colorKey: string; label: string }>();
  for (const row of rows) {
    const traceKey = [
      labelPart(row, 'analysis_id', '(analysis)'),
      labelPart(row, 'source_frame_index', 'frame ?'),
      labelPart(row, 'time_seconds', labelPart(row, 'frame_time_value', 'time ?')),
      labelPart(row, 'spectrum_id', `spectrum ${labelPart(row, 'spectrum_index', '?')}`),
    ].join(' · ');
    const colorKey = colorGrouping.map((column) => labelPart(row, column, '(empty)')).join(' · ');
    const group = groups.get(traceKey) ?? { rows: [], colorKey, label: spectrumLabel(row) };
    group.rows.push(row);
    groups.set(traceKey, group);
  }

  const colorKeys = [...new Set([...groups.values()].map((group) => group.colorKey))].sort();
  const colors = new Map(colorKeys.map((key, index) => [key, `hsl(${(index * 137.508) % 360} 65% 45%)`]));
  const legendGroups = new Set<string>();
  const traces = [...groups.values()].map((group) => {
    const sorted = [...group.rows].sort((left, right) => number(left, 'point_index') - number(right, 'point_index'));
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
      x: sorted.map((row) => number(row, 'spectral_coordinate')),
      y: sorted.map((row) => number(row, 'intensity')),
      customdata: sorted.map((row) => text(row, 'row_key')),
      hovertemplate: `${group.label}<br>Color group: ${group.colorKey}<br>Coordinate: %{x}<br>Intensity: %{y}<extra></extra>`,
    };
  });

  const first = rows[0];
  const axisLabel = [labelPart(first, 'axis_kind', 'Spectral coordinate'), text(first, 'axis_unit')]
    .filter(Boolean)
    .join(' (')
    .replace(/ \($/, '')
    .concat(text(first, 'axis_unit') ? ')' : '');
  const signalLabel = [labelPart(first, 'signal_kind', 'Intensity'), text(first, 'signal_unit')]
    .filter(Boolean)
    .join(' (')
    .replace(/ \($/, '')
    .concat(text(first, 'signal_unit') ? ')' : '');

  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'raman.spectra_table_viewer',
    semantic_type: 'raman.spectra',
    title: 'Raman spectra',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 70, r: 20, t: 20, b: 55 },
        xaxis: {
          title: { text: axisLabel, font: { color: theme.text } },
          showgrid: true,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          ...(viewportRange ? { range: viewportRange } : {}),
        },
        yaxis: {
          title: { text: signalLabel, font: { color: theme.text } },
          showgrid: true,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
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
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.raman.spectra_table_viewer',
      producer_node_id: 'frontend.raman.spectra_table_viewer',
    },
    fallback: { description: 'Raman spectra visualization unavailable.' },
  };
}

export function RamanSpectraViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<ArtifactDataResponse | null>(null);
  const [grouping, setGrouping] = useState<string[]>(DEFAULT_GROUP);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [themeRevision, setThemeRevision] = useState(0);
  const [viewportRange, setViewportRange] = useState<[number, number] | null>(null);
  const viewportTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const artifactId = artifact?.artifact_id ?? '';

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision((current) => current + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!artifact || !client) return;
    let cancelled = false;
    setLoading(true);
    setError(null);
    const loadRows = async () => {
      const response = await client.artifactQuery(context.sessionId, {
        artifact_id: artifact.artifact_id,
        mode: 'visualization',
        columns: [
          'analysis_id',
          'spectrum_id',
          'spectrum_index',
          'source_frame_index',
          'frame_time_value',
          'point_index',
          'time_seconds',
          'axis_kind',
          'axis_unit',
          'signal_kind',
          'signal_unit',
          'spectral_coordinate',
          'intensity',
        ],
        x_column: 'spectral_coordinate',
        y_column: 'intensity',
        trace_columns: ['analysis_id', 'source_frame_index', 'time_seconds', 'frame_time_value', 'spectrum_id'],
        // Raman files can contain many time-resolved spectra. Keep the first
        // five spectra from every analysis in the viewer; each selected
        // spectrum still retains its full point resolution.
        filters: [{ column: 'spectrum_index', operator: 'lte', value: 4 }],
        // Keep the first render lightweight. A zoomed spectral window loads
        // the higher-resolution traces on demand through the same query.
        max_points_per_trace: viewportRange ? VIEWPORT_MAX_POINTS_PER_TRACE : INITIAL_MAX_POINTS_PER_TRACE,
        limit: viewportRange ? VIEWPORT_MAX_ROWS : INITIAL_MAX_ROWS,
        ...(viewportRange ? { x_min: viewportRange[0], x_max: viewportRange[1] } : {}),
      });
      if (!cancelled) setData({ ...response, offset: 0, limit: response.rows.length });
    };
    void loadRows()
      .catch((reason: unknown) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Unable to load Raman spectra rows.');
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId, viewportRange]);

  useEffect(
    () => () => {
      if (viewportTimer.current) clearTimeout(viewportTimer.current);
    },
    [],
  );

  const handleRelayout = useCallback((event: VisualizationRelayout) => {
    if (event['xaxis.autorange'] === true) {
      setViewportRange(null);
      return;
    }
    const min = Number(event['xaxis.range[0]']);
    const max = Number(event['xaxis.range[1]']);
    if (!Number.isFinite(min) || !Number.isFinite(max) || min >= max) return;
    if (viewportTimer.current) clearTimeout(viewportTimer.current);
    viewportTimer.current = setTimeout(() => setViewportRange([min, max]), 180);
  }, []);

  const rows = data?.rows ?? [];
  const theme = useMemo(() => getPlotTheme(), [themeRevision]);
  const spec = useMemo(
    () => (rows.length ? makeSpec(rows, grouping, artifactId, theme, viewportRange) : null),
    [artifactId, grouping, rows, theme, viewportRange],
  );

  if (!artifact || !client) return <div role="alert">Raman spectra artifact data is unavailable.</div>;
  if (loading && !data) return <LoadingLogo label="Loading Raman spectra rows…" />;
  if (error)
    return (
      <div className="sf-raman-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!rows.length || !spec) return <div className="sf-raman-viewer-state">No Raman spectra rows are available.</div>;

  return (
    <div className="sf-raman-viewer">
      <div className="sf-raman-viewer-toolbar" aria-label="Raman spectra trace controls">
        <span className="sf-raman-viewer-toolbar-label">Color/group traces by</span>
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
        <span className="sf-raman-viewer-summary">
          {rows.length} displayed rows
          {data && data.total_rows > rows.length ? ` of ${data.total_rows.toLocaleString()}` : ''} ·{' '}
          {spec.payload.data.length} traces{loading ? ' · loading viewport…' : ''}
        </span>
      </div>
      <div className="sf-raman-viewer-plot">
        <VisualizationRenderer spec={spec} onRelayout={handleRelayout} />
      </div>
    </div>
  );
}
