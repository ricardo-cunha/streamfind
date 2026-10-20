import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type CSSProperties,
  type PointerEvent as ReactPointerEvent,
} from 'react';
import type { ArtifactRecord } from '../../framework/backend/StreamFindApiClient';
import { localArtifactContract } from '../../framework/artifacts/artifactContracts';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationPointClick } from '../../framework/visualization/VisualizationRegistry';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import {
  buildStatisticsPanels,
  STATISTICS_ARTIFACT_CONTRACTS,
  STATISTICS_RESULT_CONTRACTS,
  STATISTICS_TABLE_CONTRACTS,
  getStatisticsSelection,
  type StatisticsPlotTheme,
  type StatisticsRow,
  type StatisticsRows,
} from './statisticsPlots';
import './StatisticsDashboard.css';

const PLOT_ROW_LIMIT = 5000;

const STATISTICS_PLOT_COLUMNS: Record<string, string[]> = {
  pcaScoresTable: ['sample_id', 'component', 'score', 'group_label'],
  pcaLoadingsTable: ['variable_id', 'component', 'loading'],
  pcaVarianceTable: ['component', 'explained_variance_ratio'],
  plsScoresTable: ['sample_id', 'component', 'x_score', 'y_score', 'group_label'],
  plsLoadingsTable: ['block', 'item_id', 'component', 'loading'],
  plsCoefficientsTable: ['response_id', 'predictor_id', 'coefficient'],
  plsPredictionsTable: ['sample_id', 'observed_value', 'predicted_value', 'confidence', 'predicted_label', 'observed_label'],
  foldChangeTable: ['variable_id', 'log2_fold_change', 'p_value', 'significant'],
};

function requiredPlotTables(resultContract: string): string[] {
  if (resultContract === 'pcaDiagnostics') return ['pcaScoresTable', 'pcaLoadingsTable', 'pcaVarianceTable'];
  if (resultContract === 'plsDiagnostics')
    return ['plsScoresTable', 'plsPredictionsTable', 'plsLoadingsTable', 'plsCoefficientsTable'];
  if (resultContract === 'foldChangeDiagnostics') return ['foldChangeTable'];
  return [...STATISTICS_TABLE_CONTRACTS];
}

function isStatisticsArtifact(artifact: ArtifactRecord): boolean {
  return (STATISTICS_ARTIFACT_CONTRACTS as readonly string[]).includes(localArtifactContract(artifact.contract_id));
}

function plotTheme(): StatisticsPlotTheme {
  const get = (token: string, fallback: string) =>
    typeof document === 'undefined'
      ? fallback
      : getComputedStyle(document.documentElement).getPropertyValue(token).trim() || fallback;
  return {
    text: get('--sf-text', '#30343b'),
    grid: get('--sf-border', '#d5d9df'),
    background: get('--sf-surface-raised', '#ffffff'),
    accent: get('--sf-accent', '#3874c8'),
  };
}

function payloadObject(artifact: ArtifactRecord | undefined): Record<string, unknown> {
  if (!artifact) return {};
  if (artifact.payload && typeof artifact.payload === 'object' && !Array.isArray(artifact.payload))
    return artifact.payload as Record<string, unknown>;
  if (typeof artifact.payload === 'string') {
    try {
      const parsed: unknown = JSON.parse(artifact.payload);
      return parsed && typeof parsed === 'object' && !Array.isArray(parsed)
        ? (parsed as Record<string, unknown>)
        : {};
    } catch {
      return {};
    }
  }
  return {};
}

function selectionForRow(row: StatisticsRow): string | null {
  return row.sample_id ?? row.variable_id ?? row.item_id ?? row.predictor_id ?? null;
}

export function StatisticsDashboard({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [siblings, setSiblings] = useState<ArtifactRecord[]>([]);
  const [rowsByContract, setRowsByContract] = useState<StatisticsRows>({});
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [activePanelId, setActivePanelId] = useState('');
  const [themeRevision, setThemeRevision] = useState(0);
  const [controlsWidth, setControlsWidth] = useState(240);
  const [detailsWidth, setDetailsWidth] = useState(380);
  const workspaceRef = useRef<HTMLDivElement>(null);
  const artifactId = artifact?.artifact_id ?? '';

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision((revision) => revision + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!artifact || !client) {
      setLoading(false);
      setError(!client ? 'Statistics viewer is not connected to the data service.' : null);
      return;
    }
    let cancelled = false;
    setLoading(true);
    setError(null);
    void client
      .currentArtifacts(context.sessionId)
      .then((current) => {
        const matching = current.filter(
          (candidate) => isStatisticsArtifact(candidate) && candidate.producer_instance === artifact.producer_instance,
        );
        if (!matching.some((candidate) => candidate.artifact_id === artifact.artifact_id)) matching.unshift(artifact);
        const requiredTables = new Set(requiredPlotTables(localArtifactContract(artifact.contract_id)));
        return Promise.all(
          matching.filter((candidate) => requiredTables.has(localArtifactContract(candidate.contract_id))).map(async (candidate) => {
            const contract = localArtifactContract(candidate.contract_id);
            const result = await client.artifactQuery(context.sessionId, {
              artifact_id: candidate.artifact_id,
              mode: 'page',
              columns: STATISTICS_PLOT_COLUMNS[contract],
              offset: 0,
              limit: PLOT_ROW_LIMIT,
            });
            return [contract, result.rows] as const;
          }),
        ).then((loaded) => ({ matching, loaded }));
      })
      .then(({ matching, loaded }) => {
        if (cancelled) return;
        const rows: StatisticsRows = {};
        for (const [contract, values] of loaded) {
          if ((STATISTICS_TABLE_CONTRACTS as readonly string[]).includes(contract))
            rows[contract as (typeof STATISTICS_TABLE_CONTRACTS)[number]] = values;
        }
        setSiblings(matching);
        setRowsByContract(rows);
      })
      .catch((reason: unknown) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Unable to load statistics results.');
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId]);

  const panels = useMemo(
    () => buildStatisticsPanels(rowsByContract, artifactId, plotTheme(), selectedId),
    [rowsByContract, artifactId, selectedId, themeRevision],
  );
  useEffect(() => {
    if (!panels.some((panel) => panel.id === activePanelId)) setActivePanelId(panels[0]?.id ?? '');
  }, [activePanelId, panels]);

  const activePanel = panels.find((panel) => panel.id === activePanelId) ?? panels[0];
  const selectedDetails = useMemo(() => {
    if (!selectedId) return null;
    for (const rows of Object.values(rowsByContract)) {
      const match = rows?.find((row) => selectionForRow(row) === selectedId);
      if (match) return match;
    }
    return null;
  }, [rowsByContract, selectedId]);
  const diagnosticArtifacts = siblings.filter((candidate) =>
    (STATISTICS_RESULT_CONTRACTS as readonly string[]).includes(localArtifactContract(candidate.contract_id)),
  );
  const summary = payloadObject(artifact ?? diagnosticArtifacts[0]);

  if (loading) return <LoadingLogo label="Loading statistics plot…" />;
  if (error)
    return <div className="sf-statistics-state sf-statistics-error" role="alert">{error}</div>;
  if (!panels.length)
    return <div className="sf-statistics-state" role="status">No plot-ready statistics results are available.</div>;

  const handlePointClick = (point: VisualizationPointClick) => {
    const selection = getStatisticsSelection(point);
    if (selection) setSelectedId(selection);
  };

  const resizeControls = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    const move = (moveEvent: PointerEvent) => {
      const bounds = workspaceRef.current?.getBoundingClientRect();
      if (!bounds) return;
      const maximum = Math.max(280, bounds.width - 360 - 6 - 6 - detailsWidth);
      setControlsWidth(Math.max(200, Math.min(maximum, moveEvent.clientX - bounds.left)));
    };
    const stop = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', stop);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', stop, { once: true });
  };

  const resizeDetails = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    const move = (moveEvent: PointerEvent) => {
      const bounds = workspaceRef.current?.getBoundingClientRect();
      if (!bounds) return;
      const maximum = Math.max(320, bounds.width - controlsWidth - 360 - 6 - 6);
      setDetailsWidth(Math.max(280, Math.min(maximum, bounds.right - moveEvent.clientX)));
    };
    const stop = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', stop);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', stop, { once: true });
  };

  return (
    <main className="sf-statistics-dashboard" aria-label="Statistics plot viewer">
      <div
        ref={workspaceRef}
        className="sf-statistics-workspace"
        style={
          {
            '--sf-statistics-controls-width': `${controlsWidth}px`,
            '--sf-statistics-details-width': `${detailsWidth}px`,
          } as CSSProperties
        }
      >
        <aside className="sf-statistics-controls-panel" aria-label="Statistics plot controls">
          <div className="sf-statistics-panel-heading">
            <strong>Controls</strong>
            <span>{panels.length} plots</span>
          </div>
          <label>
            Plot
            <select value={activePanel?.id ?? ''} onChange={(event) => setActivePanelId(event.target.value)}>
              {panels.map((panel) => <option key={panel.id} value={panel.id}>{panel.title}</option>)}
            </select>
          </label>
          <details className="sf-statistics-json" open>
            <summary>Analysis summary</summary>
            <pre>{JSON.stringify(summary, null, 2)}</pre>
          </details>
          {diagnosticArtifacts.map((candidate) => (
            <details className="sf-statistics-json" key={candidate.artifact_id}>
              <summary>{localArtifactContract(candidate.contract_id)} diagnostics</summary>
              <pre>{JSON.stringify(payloadObject(candidate), null, 2) || '{}'}</pre>
            </details>
          ))}
        </aside>
        <div
          className="sf-statistics-splitter sf-statistics-controls-splitter"
          role="separator"
          aria-label="Resize statistics controls panel"
          aria-orientation="vertical"
          onPointerDown={resizeControls}
        />
        <section className="sf-statistics-plot-panel" aria-label="Statistics plot">
          <div className="sf-statistics-plot-stage">
            {activePanel && (
              <VisualizationRenderer
                spec={activePanel.spec}
                className="sf-statistics-plot"
                onPointClick={activePanel.selectionKind === 'none' ? undefined : handlePointClick}
                onDoubleClick={() => setSelectedId(null)}
              />
            )}
          </div>
        </section>
        <div
          className="sf-statistics-splitter sf-statistics-details-splitter"
          role="separator"
          aria-label="Resize statistics details panel"
          aria-orientation="vertical"
          onPointerDown={resizeDetails}
        />
        <aside className="sf-statistics-details-panel" aria-label="Statistics selected result details">
          <div className="sf-statistics-panel-heading">
            <strong>Details</strong>
            <span>{selectedId ? 'Selection' : 'No selection'}</span>
          </div>
          {selectedDetails ? (
            <section className="sf-statistics-selection" aria-label="Selected result details">
              <h3>Selected result: {selectedId}</h3>
              <dl>
                {Object.entries(selectedDetails).map(([key, value]) => (
                  <div key={key}><dt>{key}</dt><dd>{value ?? '—'}</dd></div>
                ))}
              </dl>
            </section>
          ) : (
            <p className="sf-statistics-empty">Select a point in the plot to inspect its values.</p>
          )}
        </aside>
      </div>
    </main>
  );
}
