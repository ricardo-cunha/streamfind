import { useEffect, useMemo, useState } from 'react';
import type { ArtifactDataResponse } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';

type EicRow = Record<string, string | null>;

const GROUP_COLUMNS = ['analysis', 'name', 'replicate', 'polarity', 'level'] as const;
const DEFAULT_GROUP = ['analysis', 'name'];

function text(row: EicRow, column: string): string {
  return row[column] ?? '';
}

function number(row: EicRow, column: string): number {
  const value = Number(row[column]);
  return Number.isFinite(value) ? value : 0;
}

function makeSpec(rows: EicRow[], colorGrouping: string[], artifactId: string): VisualizationSpec {
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
        xaxis: { title: 'Retention time (s)' },
        yaxis: { title: 'Intensity' },
        hovermode: 'closest',
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.mass_spec.eic_table_viewer',
      producer_node_id: 'frontend.mass_spec.eic_table_viewer',
    },
    fallback: { description: 'EIC visualization unavailable.' },
  };
}

export function EicTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<ArtifactDataResponse | null>(null);
  const [grouping, setGrouping] = useState<string[]>(DEFAULT_GROUP);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const artifactId = artifact?.artifact_id ?? '';

  useEffect(() => {
    if (!artifact || !client) return;
    let cancelled = false;
    setLoading(true);
    setError(null);
    const loadAllRows = async () => {
      const columns = ['analysis', 'replicate', 'name', 'polarity', 'level', 'rt', 'intensity'];
      const response = await client.artifactQuery(context.sessionId, {
        artifact_id: artifact.artifact_id,
        mode: 'eic',
        columns,
        limit: 250000,
        eic_max_points: 512,
      });
      if (!cancelled) setData({ ...response, offset: 0, limit: response.rows.length });
    };
    void loadAllRows()
      .catch((reason: unknown) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Unable to load EIC rows.');
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId]);

  const rows = data?.rows ?? [];
  const spec = useMemo(() => (rows.length ? makeSpec(rows, grouping, artifactId) : null), [artifactId, grouping, rows]);

  if (!artifact || !client) return <div role="alert">EIC artifact data is unavailable.</div>;
  if (loading) return <LoadingLogo label="Loading EIC rows…" />;
  if (error)
    return (
      <div className="sf-eic-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!rows.length || !spec) return <div className="sf-eic-viewer-state">No EIC rows are available.</div>;

  return (
    <div className="sf-eic-viewer">
      <div className="sf-eic-viewer-toolbar" aria-label="EIC trace controls">
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
        <span className="sf-eic-viewer-summary">
          {rows.length} rows · {spec.payload.data.length} traces
        </span>
      </div>
      <div className="sf-eic-viewer-plot">
        <VisualizationRenderer spec={spec} />
      </div>
    </div>
  );
}
