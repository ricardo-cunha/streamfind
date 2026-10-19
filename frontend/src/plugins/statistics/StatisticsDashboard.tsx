import { useEffect, useMemo, useState } from 'react';
import type { ArtifactDataResponse, ArtifactRecord } from '../../framework/backend/StreamFindApiClient';
import { localArtifactContract } from '../../framework/artifacts/artifactContracts';
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
const TABLE_PAGE_SIZE = 50;

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

function payloadText(artifact: ArtifactRecord): string {
  const payload = artifact.payload;
  if (typeof payload === 'string') return payload;
  if (payload && typeof payload === 'object') return JSON.stringify(payload, null, 2);
  return '';
}

function selectionForRow(row: StatisticsRow): string | null {
  return row.sample_id ?? row.variable_id ?? row.item_id ?? row.predictor_id ?? null;
}

export function StatisticsDashboard({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [siblings, setSiblings] = useState<ArtifactRecord[]>([]);
  const [rowsByContract, setRowsByContract] = useState<StatisticsRows>({});
  const [tableArtifactId, setTableArtifactId] = useState('');
  const [tablePage, setTablePage] = useState<ArtifactDataResponse | null>(null);
  const [tableOffset, setTableOffset] = useState(0);
  const [sortColumn, setSortColumn] = useState('');
  const [sortDescending, setSortDescending] = useState(false);
  const [loading, setLoading] = useState(true);
  const [tableLoading, setTableLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [themeRevision, setThemeRevision] = useState(0);
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
        return Promise.all(
          matching.map(async (candidate) => {
            const contract = localArtifactContract(candidate.contract_id);
            if (!(STATISTICS_TABLE_CONTRACTS as readonly string[]).includes(contract))
              return [contract, [] as StatisticsRow[]] as const;
            const result = await client.artifactQuery(context.sessionId, {
              artifact_id: candidate.artifact_id,
              mode: 'page',
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
        const tableArtifacts = matching.filter((candidate) =>
          (STATISTICS_TABLE_CONTRACTS as readonly string[]).includes(localArtifactContract(candidate.contract_id)),
        );
        setSiblings(matching);
        setRowsByContract(rows);
        setTableArtifactId(tableArtifacts[0]?.artifact_id ?? '');
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

  useEffect(() => {
    if (!client || !tableArtifactId) {
      setTablePage(null);
      return;
    }
    let cancelled = false;
    setTableLoading(true);
    void client
      .artifactQuery(context.sessionId, {
        artifact_id: tableArtifactId,
        mode: 'page',
        offset: tableOffset,
        limit: TABLE_PAGE_SIZE,
        ...(sortColumn ? { sort_column: sortColumn, descending: sortDescending } : {}),
      })
      .then((page) => {
        if (!cancelled) setTablePage(page);
      })
      .catch((reason: unknown) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Unable to load the result table.');
      })
      .finally(() => {
        if (!cancelled) setTableLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [client, context.sessionId, tableArtifactId, tableOffset, sortColumn, sortDescending]);

  const panels = useMemo(
    () => buildStatisticsPanels(rowsByContract, artifactId, plotTheme(), selectedId),
    [rowsByContract, artifactId, selectedId, themeRevision],
  );
  const selectedDetails = useMemo(() => {
    if (!selectedId) return null;
    for (const rows of Object.values(rowsByContract)) {
      const match = rows?.find((row) => selectionForRow(row) === selectedId);
      if (match) return match;
    }
    return tablePage?.rows.find((row) => selectionForRow(row) === selectedId) ?? null;
  }, [rowsByContract, selectedId, tablePage]);
  const tableArtifact = siblings.find((candidate) => candidate.artifact_id === tableArtifactId);
  const tableContract = tableArtifact ? localArtifactContract(tableArtifact.contract_id) : '';
  const diagnosticArtifacts = siblings.filter((candidate) =>
    (STATISTICS_RESULT_CONTRACTS as readonly string[]).includes(localArtifactContract(candidate.contract_id)),
  );

  if (loading)
    return (
      <div className="sf-statistics-state" role="status">
        Loading statistics results…
      </div>
    );
  if (error)
    return (
      <div className="sf-statistics-state sf-statistics-error" role="alert">
        {error}
      </div>
    );
  if (!panels.length && !siblings.length)
    return (
      <div className="sf-statistics-state" role="status">
        No statistics results are available for this operation.
      </div>
    );

  const handlePointClick = (point: VisualizationPointClick) => {
    const selection = getStatisticsSelection(point);
    if (selection) setSelectedId(selection);
  };
  const columns = tablePage?.columns ?? [];

  return (
    <main className="sf-statistics-dashboard" aria-label="Statistics results">
      <header className="sf-statistics-header">
        <div>
          <h2>Statistics results</h2>
          <p>Plots and result tables from this analysis are linked by sample and variable ID.</p>
        </div>
        {selectedId && (
          <button type="button" className="sf-statistics-clear-selection" onClick={() => setSelectedId(null)}>
            Clear selection · {selectedId}
          </button>
        )}
      </header>

      {panels.length > 0 ? (
        <section className="sf-statistics-plots" aria-label="Result plots">
          {panels.map((plot) => (
            <article className="sf-statistics-plot-card" key={plot.id}>
              <h3>{plot.title}</h3>
              <VisualizationRenderer
                spec={plot.spec}
                className="sf-statistics-plot"
                onPointClick={plot.selectionKind === 'none' ? undefined : handlePointClick}
              />
            </article>
          ))}
        </section>
      ) : (
        <div className="sf-statistics-state" role="status">
          This analysis has no plot-ready result rows.
        </div>
      )}

      <section className="sf-statistics-lower-panels">
        <article className="sf-statistics-table-card">
          <div className="sf-statistics-section-heading">
            <h3>Result table</h3>
            <label>
              <span>Output</span>
              <select
                value={tableArtifactId}
                onChange={(event) => {
                  setTableArtifactId(event.target.value);
                  setTableOffset(0);
                }}
              >
                {siblings
                  .filter((candidate) =>
                    (STATISTICS_TABLE_CONTRACTS as readonly string[]).includes(
                      localArtifactContract(candidate.contract_id),
                    ),
                  )
                  .map((candidate) => (
                    <option key={candidate.artifact_id} value={candidate.artifact_id}>
                      {localArtifactContract(candidate.contract_id)}
                    </option>
                  ))}
              </select>
            </label>
          </div>
          {!tableArtifact ? (
            <p className="sf-statistics-muted">No table output is available.</p>
          ) : tableLoading ? (
            <p className="sf-statistics-muted" role="status">
              Loading table page…
            </p>
          ) : tablePage && tablePage.rows.length > 0 ? (
            <>
              <div className="sf-statistics-table-scroll">
                <table>
                  <thead>
                    <tr>
                      {columns.map((column) => (
                        <th key={column.name}>
                          <button
                            type="button"
                            className="sf-statistics-sort"
                            onClick={() => {
                              setTableOffset(0);
                              if (sortColumn === column.name) setSortDescending((descending) => !descending);
                              else {
                                setSortColumn(column.name);
                                setSortDescending(false);
                              }
                            }}
                          >
                            {column.name}
                            {sortColumn === column.name ? (sortDescending ? ' ↓' : ' ↑') : ''}
                          </button>
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {tablePage.rows.map((row, index) => {
                      const id = selectionForRow(row);
                      return (
                        <tr
                          key={`${id ?? 'row'}-${index}`}
                          className={id === selectedId ? 'is-selected' : ''}
                          onClick={() => id && setSelectedId(id)}
                        >
                          {columns.map((column) => (
                            <td key={column.name}>{row[column.name] ?? '—'}</td>
                          ))}
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
              <div className="sf-statistics-pagination">
                <span>
                  {tablePage.total_rows
                    ? `${tableOffset + 1}–${Math.min(tableOffset + tablePage.rows.length, tablePage.total_rows)} of ${tablePage.total_rows}`
                    : 'No rows'}
                </span>
                <button
                  type="button"
                  disabled={tableOffset === 0 || tableLoading}
                  onClick={() => setTableOffset(Math.max(0, tableOffset - TABLE_PAGE_SIZE))}
                >
                  Previous
                </button>
                <button
                  type="button"
                  disabled={tableOffset + TABLE_PAGE_SIZE >= tablePage.total_rows || tableLoading}
                  onClick={() => setTableOffset(tableOffset + TABLE_PAGE_SIZE)}
                >
                  Next
                </button>
              </div>
            </>
          ) : (
            <p className="sf-statistics-muted" role="status">
              This result table is empty.
            </p>
          )}
          {tableContract && <span className="sf-statistics-contract">{tableContract}</span>}
        </article>

        <article className="sf-statistics-details-card">
          <h3>Details</h3>
          {selectedDetails ? (
            <dl>
              {Object.entries(selectedDetails).map(([key, value]) => (
                <div key={key}>
                  <dt>{key}</dt>
                  <dd>{value ?? '—'}</dd>
                </div>
              ))}
            </dl>
          ) : selectedId ? (
            <p className="sf-statistics-muted">No row details are available for {selectedId}.</p>
          ) : (
            <p className="sf-statistics-muted">Select a point or result row to inspect its values.</p>
          )}
          {diagnosticArtifacts.map((candidate) => (
            <details key={candidate.artifact_id}>
              <summary>{localArtifactContract(candidate.contract_id)}</summary>
              <pre>{payloadText(candidate) || 'No diagnostics were returned.'}</pre>
            </details>
          ))}
        </article>
      </section>
    </main>
  );
}
