import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';

type SuspectRow = Record<string, string | null>;
type RenderedStructure = { svg: string; dataUri: string };
type StructureCache = Record<string, RenderedStructure>;

function value(row: SuspectRow, names: string[]): string | null {
  for (const name of names) {
    const candidate = row[name];
    if (candidate !== null && candidate !== undefined && candidate !== '') return candidate;
  }
  return null;
}

function parseNumbers(raw: string | null): number[] {
  if (!raw) return [];
  try {
    const parsed: unknown = JSON.parse(raw);
    if (Array.isArray(parsed)) return parsed.map(Number).filter(Number.isFinite);
  } catch {
    // Some table transports expose arrays as a JSON string nested in a string.
    try {
      const parsed: unknown = JSON.parse(JSON.parse(raw) as string);
      if (Array.isArray(parsed)) return parsed.map(Number).filter(Number.isFinite);
    } catch {
      return [];
    }
  }
  return [];
}

function svgDataUri(svg: string): string {
  const normalized = svg.replace(/^\s*<\?xml[^>]*>\s*/i, '').replace(/^\s*<!DOCTYPE[^>]*>\s*/i, '');
  const bytes = new TextEncoder().encode(normalized);
  let binary = '';
  bytes.forEach((byte) => {
    binary += String.fromCharCode(byte);
  });
  return `data:image/svg+xml;base64,${btoa(binary)}`;
}

function displayMass(row: SuspectRow): string {
  const mass = value(row, ['mass', 'exact_mass']);
  if (!mass || !Number.isFinite(Number(mass))) return 'Mass unavailable';
  return `${Number(mass).toFixed(5)} Da`;
}

function structureCacheKey(row: SuspectRow): string {
  return `${value(row, ['SMILES', 'smiles']) ?? ''}\\u0000${value(row, ['InChI', 'inchi']) ?? ''}`;
}

function StructureImage({
  row,
  large,
  client,
  cache,
  onRendered,
}: {
  row: SuspectRow;
  large: boolean;
  client: StreamFindApiClient;
  cache: StructureCache;
  onRendered: (key: string, structure: RenderedStructure) => void;
}) {
  const cacheKey = structureCacheKey(row);
  const [structure, setStructure] = useState<RenderedStructure | null>(() => cache[cacheKey] ?? null);
  const [error, setError] = useState(false);
  const smiles = value(row, ['SMILES', 'smiles']);
  const inchi = value(row, ['InChI', 'inchi']);
  useEffect(() => {
    let active = true;
    if (!smiles && !inchi) {
      return () => {
        active = false;
      };
    }
    if (cache[cacheKey]) {
      return () => {
        active = false;
      };
    }
    void client
      // Use one Open Babel render size for cards and details. The detail view
      // scales this SVG with CSS instead of requesting a second oversized render.
      .structureSvg({ smiles, inchi, width: 300, height: 240 })
      .then((svg) => {
        if (active) {
          const rendered = { svg, dataUri: svgDataUri(svg) };
          setStructure(rendered);
          onRendered(cacheKey, rendered);
        }
      })
      .catch(() => {
        if (active) setError(true);
      });
    return () => {
      active = false;
    };
  }, [cache, cacheKey, client, inchi, large, onRendered, smiles]);
  const displayedStructure = structure ?? cache[cacheKey] ?? null;
  if (displayedStructure)
    return (
      <img
        className={large ? 'sf-suspect-structure-large' : 'sf-suspect-structure'}
        src={displayedStructure.dataUri}
        alt={`Chemical structure for ${value(row, ['name']) ?? 'compound'}`}
      />
    );
  return (
    <div className="sf-suspect-structure-placeholder">
      {(!smiles && !inchi) || error ? 'Structure unavailable' : 'Rendering structure…'}
    </div>
  );
}

function ms2Spec(
  row: SuspectRow,
  polarity: 'pos' | 'neg',
  darkMode: boolean,
  sourceArtifactId: string,
): VisualizationSpec | null {
  const mz = parseNumbers(value(row, [`fragments_mz_${polarity}`]));
  const intensity = parseNumbers(value(row, [`fragments_intensity_${polarity}`]));
  const count = Math.min(mz.length, intensity.length);
  if (!count) return null;
  const plotTheme = darkMode
    ? { background: '#111', grid: '#39414d', text: '#f3f4f6' }
    : { background: '#fff', grid: '#d7dce2', text: '#1f2937' };
  const x = mz.slice(0, count);
  const y = intensity.slice(0, count);
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: `suspect-ms2-${polarity}`,
    semantic_type: 'sfms:ms2',
    title: `MS2 (${polarity === 'pos' ? 'positive' : 'negative'})`,
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: [
        {
          type: 'scatter',
          mode: 'lines',
          x: x.flatMap((item) => [item, item, null]),
          y: y.flatMap((item) => [0, item, null]),
          line: { color: '#1f77b4', width: 2 },
          hovertemplate: 'm/z %{x:.4f}<br>intensity %{y:.4f}<extra></extra>',
        },
      ],
      layout: {
        autosize: true,
        showlegend: false,
        margin: { l: 55, r: 20, t: 35, b: 50 },
        annotations: x.map((item, index) => ({
          x: item,
          y: y[index],
          text: item.toFixed(4),
          showarrow: false,
          yshift: 8,
          font: { color: plotTheme.text, size: 10 },
        })),
        xaxis: {
          title: { text: 'm/z' },
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          titlefont: { color: plotTheme.text },
          tickfont: { color: plotTheme.text },
        },
        yaxis: {
          title: { text: 'Intensity' },
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          titlefont: { color: plotTheme.text },
          tickfont: { color: plotTheme.text },
        },
        paper_bgcolor: plotTheme.background,
        plot_bgcolor: plotTheme.background,
        font: { color: plotTheme.text },
      },
      config: { displaylogo: false, responsive: true, displayModeBar: true },
    },
    provenance: {
      source_artifact_ids: [sourceArtifactId],
      producer_operation_id: 'frontend.suspect-targets-viewer',
      producer_node_id: 'suspect-targets-viewer',
    },
    fallback: { description: 'The MS2 renderer is unavailable.' },
  };
}

function isotopeSpec(
  mz: number[],
  probability: number[],
  darkMode: boolean,
  sourceArtifactId: string,
  labels: string[],
): VisualizationSpec | null {
  const count = Math.min(mz.length, probability.length);
  if (!count) return null;
  const plotTheme = darkMode
    ? { background: '#111', grid: '#39414d', text: '#f3f4f6' }
    : { background: '#fff', grid: '#d7dce2', text: '#1f2937' };
  const x = mz.slice(0, count);
  const y = probability.slice(0, count);
  const maxProbability = Math.max(...y, 0);
  const normalized = maxProbability > 0 ? y.map((item) => item / maxProbability) : y;
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'suspect-isotope-pattern',
    semantic_type: 'sfms:isotope-pattern',
    title: 'Expected isotope pattern',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: [
        {
          type: 'scatter',
          mode: 'lines',
          x: x.flatMap((item) => [item, item, null]),
          y: normalized.flatMap((item) => [0, item, null]),
          line: { color: '#b45309', width: 2 },
          hovertemplate: 'm/z %{x:.5f}<br>relative abundance %{y:.4f}<extra></extra>',
        },
      ],
      layout: {
        autosize: true,
        showlegend: false,
        margin: { l: 55, r: 20, t: 35, b: 50 },
        annotations: x.map((item, index) => ({
          x: item,
          y: normalized[index],
          text: `${labels[index] ?? 'M'}<br>${item.toFixed(5)}`,
          showarrow: false,
          yshift: 8,
          font: { color: plotTheme.text, size: 10 },
        })),
        xaxis: { title: { text: 'm/z' }, gridcolor: plotTheme.grid, tickfont: { color: plotTheme.text } },
        yaxis: {
          title: { text: 'Relative abundance' },
          range: [0, 1.12],
          gridcolor: plotTheme.grid,
          tickfont: { color: plotTheme.text },
        },
        paper_bgcolor: plotTheme.background,
        plot_bgcolor: plotTheme.background,
        font: { color: plotTheme.text },
      },
      config: { displaylogo: false, responsive: true, displayModeBar: true },
    },
    provenance: {
      source_artifact_ids: [sourceArtifactId],
      producer_operation_id: 'frontend.suspect-targets-viewer',
      producer_node_id: 'suspect-targets-viewer',
    },
    fallback: { description: 'The isotope-pattern renderer is unavailable.' },
  };
}

function IsotopePattern({
  row,
  client,
  darkMode,
  sourceArtifactId,
}: {
  row: SuspectRow;
  client: StreamFindApiClient;
  darkMode: boolean;
  sourceArtifactId: string;
}) {
  const formula = value(row, ['formula', 'Formula']);
  const [pattern, setPattern] = useState<{ mz: number[]; probability: number[]; labels: string[] } | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let active = true;
    if (!formula)
      return () => {
        active = false;
      };
    void client
      .isotopePattern({ formula, max_peaks: 16 })
      .then((result) => {
        if (active) setPattern(result);
      })
      .catch((reason: unknown) => {
        if (active) setError(reason instanceof Error ? reason.message : 'Could not calculate isotope pattern.');
      });
    return () => {
      active = false;
    };
  }, [client, formula]);
  if (!formula) return null;
  const spec = pattern
    ? isotopeSpec(pattern.mz, pattern.probability, darkMode, sourceArtifactId, pattern.labels)
    : null;
  return (
    <section className="sf-suspect-isotope">
      <h3>Expected isotope pattern</h3>
      {error ? (
        <p role="alert">{error}</p>
      ) : spec ? (
        <VisualizationRenderer spec={spec} className="sf-suspect-isotope-plot" />
      ) : (
        <p>Calculating isotope pattern…</p>
      )}
    </section>
  );
}

function Details({
  row,
  client,
  darkMode,
  sourceArtifactId,
  cache,
  onRendered,
}: {
  row: SuspectRow;
  client: StreamFindApiClient;
  darkMode: boolean;
  sourceArtifactId: string;
  cache: StructureCache;
  onRendered: (key: string, structure: RenderedStructure) => void;
}) {
  const fields = useMemo(() => Object.entries(row).filter(([, item]) => item !== null && item !== ''), [row]);
  const positiveSpec = ms2Spec(row, 'pos', darkMode, sourceArtifactId);
  const negativeSpec = ms2Spec(row, 'neg', darkMode, sourceArtifactId);
  return (
    <div className="sf-suspect-detail" role="dialog" aria-label={`${value(row, ['name']) ?? 'Compound'} details`}>
      <div className="sf-suspect-detail-grid">
        <div className="sf-suspect-detail-structure">
          <StructureImage row={row} large client={client} cache={cache} onRendered={onRendered} />
        </div>
        <div className="sf-suspect-properties">
          {fields.map(([key, item]) => (
            <div className="sf-suspect-property" key={key}>
              <span>{key}</span>
              <code>{item}</code>
            </div>
          ))}
        </div>
      </div>
      <IsotopePattern row={row} client={client} darkMode={darkMode} sourceArtifactId={sourceArtifactId} />
      {positiveSpec || negativeSpec ? (
        <section className="sf-suspect-ms2">
          <div className="sf-suspect-ms2-grid">
            {positiveSpec ? (
              <div className="sf-suspect-ms2-panel">
                <h4>MS2 positive spectrum</h4>
                <VisualizationRenderer spec={positiveSpec} className="sf-suspect-ms2-plot" />
              </div>
            ) : null}
            {negativeSpec ? (
              <div className="sf-suspect-ms2-panel">
                <h4>MS2 negative spectrum</h4>
                <VisualizationRenderer spec={negativeSpec} className="sf-suspect-ms2-plot" />
              </div>
            ) : null}
          </div>
        </section>
      ) : null}
    </div>
  );
}

export function SuspectTargetsViewer({ context }: ViewerComponentProps): ReactNode {
  const [rows, setRows] = useState<SuspectRow[]>([]);
  const [selected, setSelected] = useState<SuspectRow | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [darkMode, setDarkMode] = useState(() => document.documentElement.dataset.theme === 'dark');
  const [structureCache, setStructureCache] = useState<StructureCache>({});
  const client = context.pluginApi?.client;
  const rememberStructure = useCallback((key: string, structure: RenderedStructure) => {
    setStructureCache((current) => (current[key] ? current : { ...current, [key]: structure }));
  }, []);
  useEffect(() => {
    const observer = new MutationObserver(() => setDarkMode(document.documentElement.dataset.theme === 'dark'));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    if (!selected) return undefined;
    const handleEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      event.stopPropagation();
      setSelected(null);
    };
    document.addEventListener('keydown', handleEscape, true);
    return () => document.removeEventListener('keydown', handleEscape, true);
  }, [selected]);
  useEffect(() => {
    let active = true;
    if (!client || !context.artifact) {
      void Promise.resolve().then(() => {
        if (!active) return;
        setLoading(false);
        setError('Suspect target data is unavailable.');
      });
      return () => {
        active = false;
      };
    }
    void client
      .artifactData(context.sessionId, { artifact_id: context.artifact.artifact_id, offset: 0, limit: 5000 })
      .then((result) => {
        if (!active) return;
        const loadedRows = Array.isArray(result.rows)
          ? result.rows.filter(
              (row): row is SuspectRow => Boolean(row) && typeof row === 'object' && !Array.isArray(row),
            )
          : [];
        setRows(loadedRows);
        setLoading(false);
      })
      .catch((reason: unknown) => {
        if (!active) return;
        setError(reason instanceof Error ? reason.message : 'Could not load suspect targets.');
        setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [client, context.artifact, context.sessionId]);
  if (loading) return <div className="sf-suspect-viewer-state">Loading suspect targets…</div>;
  if (error)
    return (
      <div className="sf-suspect-viewer-state" role="alert">
        {error}
      </div>
    );
  if (!client) return <div className="sf-suspect-viewer-state">Suspect target viewer is unavailable.</div>;
  if (!rows.length) return <div className="sf-suspect-viewer-state">No suspect compounds are available.</div>;
  return (
    <div className={`sf-suspect-viewer${selected ? ' sf-suspect-viewer--modal-open' : ''}`}>
      <div className="sf-suspect-card-grid">
        {rows.map((row, index) => (
          <button
            type="button"
            className="sf-suspect-card"
            key={`${value(row, ['InChIKey', 'SMILES', 'name']) ?? 'compound'}-${index}`}
            onClick={() => setSelected(row)}
          >
            <StructureImage
              row={row}
              large={false}
              client={client}
              cache={structureCache}
              onRendered={rememberStructure}
            />
            <strong>{value(row, ['name']) ?? 'Compound'}</strong>
            <span>{displayMass(row)}</span>
          </button>
        ))}
      </div>
      {selected && client
        ? createPortal(
            <div
              className="sf-suspect-modal-backdrop"
              onMouseDown={(event) => {
                if (event.target === event.currentTarget) setSelected(null);
              }}
            >
              <div className="sf-suspect-modal">
                <header className="sf-suspect-modal-header">
                  <strong>{value(selected, ['name']) ?? 'Compound'}</strong>
                  <button type="button" className="sf-suspect-modal-close" onClick={() => setSelected(null)}>
                    Close
                  </button>
                </header>
                <div className="sf-suspect-modal-body">
                  <Details
                    row={selected}
                    client={client}
                    darkMode={darkMode}
                    sourceArtifactId={context.artifact?.artifact_id ?? ''}
                    cache={structureCache}
                    onRendered={rememberStructure}
                  />
                </div>
              </div>
            </div>,
            document.body,
          )
        : null}
    </div>
  );
}
