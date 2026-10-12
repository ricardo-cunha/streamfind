import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type CSSProperties,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from 'react';
import { createPortal } from 'react-dom';
import type { ArtifactQueryFilter, StreamFindApiClient } from '../../framework/backend/StreamFindApiClient';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import { colorFor } from './FeatureInspector';
import { StructureImage } from './SuspectTargetsViewer';

type Row = Record<string, string | null>;
type ScatterPoint = { row: Row; index: number; color: string; intensity: number };
type PlotViewport = { xMin: number; xMax: number; yMin: number; yMax: number };
type StructureCache = Record<string, { svg: string; dataUri: string }>;
type NumericFilter = { min: string; max: string };
type Column = { name: string; type?: string };

const filterDefinitions = [
  ['exp_mass', 'Experimental mass'],
  ['exp_rt', 'Experimental RT'],
  ['id_level', 'Identification level'],
  ['error_mass', 'Mass error'],
  ['error_rt', 'RT error'],
  ['cosine_similarity', 'Cosine similarity'],
  ['isotope_similarity', 'Isotope similarity'],
  ['shared_fragments', 'Shared fragments'],
] as const;

const suspectOverviewColumns = [
  'feature',
  'analysis',
  'replicate',
  'feature_group',
  'exp_mass',
  'exp_rt',
  'intensity',
  'name',
  'formula',
  'mass',
  'SMILES',
  'InChI',
  'InChIKey',
  'id_level',
  'error_mass',
  'error_rt',
  'cosine_similarity',
  'isotope_similarity',
  'shared_fragments',
  'isotope_match',
  'filtered',
] as const;

function text(row: Row, key: string): string {
  const value = row[key];
  return value === null || value === undefined ? '' : String(value);
}

function number(row: Row, key: string): number | null {
  const parsed = Number(text(row, key));
  return Number.isFinite(parsed) ? parsed : null;
}

function featureKey(row: Row): string {
  const analysis = text(row, 'analysis');
  const replicate = text(row, 'replicate');
  const feature = text(row, 'feature');
  if (feature) return `${analysis}\u0000${replicate}\u0000${feature}`;
  const group = text(row, 'feature_group');
  const mass = number(row, 'exp_mass');
  const rt = number(row, 'exp_rt');
  return `${analysis}\u0000${replicate}\u0000${group}\u0000${mass === null ? '' : mass.toFixed(6)}\u0000${rt === null ? '' : rt.toFixed(3)}`;
}

function formatValue(value: string): string {
  const numeric = Number(value);
  if (value !== '' && Number.isFinite(numeric)) return numeric.toFixed(5).replace(/0+$/, '').replace(/\.$/, '');
  return value;
}

function cssThemeValue(name: string): string {
  if (typeof document === 'undefined') return '';
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function IdentificationOverview({
  row,
  client,
  cache,
  onRendered,
}: {
  row: Row;
  client: StreamFindApiClient;
  cache: StructureCache;
  onRendered: (key: string, structure: { svg: string; dataUri: string }) => void;
}): ReactNode {
  const fields = [
    ['Identification level', 'id_level'],
    ['MetFrag score', 'score'],
    ['Database mass', 'db_mass'],
    ['Experimental mass', 'exp_mass'],
    ['Mass error (ppm)', 'error_mass'],
    ['Database RT', 'db_rt'],
    ['Experimental RT', 'exp_rt'],
    ['RT error (s)', 'error_rt'],
    ['Shared in-silico fragments', 'shared_fragments'],
    ['In-silico cosine similarity', 'cosine_similarity'],
    ['Experimental MS2 peaks', 'ms2_size'],
    ['Database MS2 peaks', 'db_ms2_size'],
    ['MS1 isotope peaks', 'isotope_matched_peaks'],
    ['MS1 isotope similarity', 'isotope_similarity'],
    ['Formula', 'formula'],
    ['InChIKey', 'InChIKey'],
  ].filter(([, key]) => text(row, key) !== '');
  return (
    <div className="sf-suspects-explorer-identification">
      <div className="sf-suspects-explorer-overview-grid">
        <div className="sf-suspects-explorer-structure">
          <StructureImage row={row} large client={client} cache={cache} onRendered={onRendered} />
        </div>
        <div className="sf-suspects-explorer-fields">
          {fields.map(([label, key]) => (
            <div className="sf-suspects-explorer-field" key={key}>
              <span>{label}</span>
              <code>{formatValue(text(row, key))}</code>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

function decodeFloatArray(encoded: string, size: string): number[] {
  if (!encoded) return [];
  try {
    const binary = atob(encoded);
    const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
    const expected = Number(size || 0);
    const precision = expected > 0 && bytes.byteLength === expected * 8 ? 8 : 4;
    if (bytes.byteLength % precision !== 0) return [];
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return Array.from({ length: bytes.byteLength / precision }, (_, index) =>
      precision === 8 ? view.getFloat64(index * precision, true) : view.getFloat32(index * precision, true),
    );
  } catch {
    return [];
  }
}

type EvidenceTab = 'eic' | 'ms1' | 'ms2';
type ModalEvidenceTab = 'ms1' | 'ms2';

function EvidencePlots({
  row,
  client,
  artifactId,
}: {
  row: Row;
  client: StreamFindApiClient;
  artifactId: string;
}): ReactNode {
  const [tab, setTab] = useState<ModalEvidenceTab>('ms1');
  const [isotope, setIsotope] = useState<{ mz: number[]; probability: number[]; labels: string[] } | null>(null);
  const formula = text(row, 'formula');
  const polarity = number(row, 'polarity');

  useEffect(() => {
    let active = true;
    if (!formula) {
      void Promise.resolve().then(() => {
        if (active) setIsotope(null);
      });
      return () => {
        active = false;
      };
    }
    void client
      .isotopePattern({
        formula,
        charge: polarity && polarity > 0 ? 1 : polarity && polarity < 0 ? -1 : 0,
        max_peaks: 16,
      })
      .then((result) => {
        if (active) setIsotope(result);
      })
      .catch(() => {
        if (active) setIsotope(null);
      });
    return () => {
      active = false;
    };
  }, [client, formula, polarity]);

  const spec = evidenceSpec(row, tab, isotope, artifactId);
  return (
    <section className="sf-suspects-explorer-evidence-panel">
      <nav className="sf-suspects-explorer-evidence-tabs" role="tablist" aria-label="Spectral evidence">
        {(['ms1', 'ms2'] as ModalEvidenceTab[]).map((item) => (
          <button
            type="button"
            role="tab"
            aria-selected={tab === item}
            className={tab === item ? 'active' : undefined}
            key={item}
            onClick={() => setTab(item)}
          >
            {`${item.toUpperCase()} mirror`}
          </button>
        ))}
      </nav>
      {spec ? (
        <VisualizationRenderer spec={spec} className="sf-suspects-explorer-evidence-plot" />
      ) : (
        <p className="sf-suspects-explorer-empty">
          No encoded {tab.toUpperCase()} evidence is available for this suspect.
        </p>
      )}
    </section>
  );
}

function SuspectScatter({
  points,
  selectedKey,
  onSelect,
  onClearSelection,
  onRelayout,
  artifactId,
}: {
  points: ScatterPoint[];
  selectedKey: string | null;
  onSelect: (key: string) => void;
  onClearSelection: () => void;
  onRelayout: (event: Record<string, unknown>) => void;
  artifactId: string;
}): ReactNode {
  const theme = {
    background: cssThemeValue('--sf-surface-raised'),
    grid: cssThemeValue('--sf-border'),
    text: cssThemeValue('--sf-text'),
  };
  const keys = points.map((point) => featureKey(point.row));
  const intensities = points.map((point) => point.intensity);
  const intensityMin = Math.min(...intensities);
  const intensityMax = Math.max(...intensities);
  const markerSizes = points.map((point) =>
    intensityMax > intensityMin ? 10 + ((point.intensity - intensityMin) / (intensityMax - intensityMin)) * 20 : 20,
  );
  const markerOpacity = selectedKey ? keys.map((key) => (key === selectedKey ? 1 : 0.2)) : 1;
  const markerLineWidth = selectedKey ? keys.map((key) => (key === selectedKey ? 2 : 0.5)) : 0.5;
  const spec: VisualizationSpec = {
    schema: 'streamfind.visualization/v1',
    visualization_id: `suspects-scatter-${artifactId}`,
    semantic_type: 'sfms:suspectFeatureScatter',
    title: 'Suspect feature map',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: [
        {
          type: 'scattergl',
          mode: 'markers',
          x: points.map((point) => Number(point.row.rt)),
          y: points.map((point) => Number(point.row.mz)),
          customdata: keys,
          text: points.map((point) => {
            const analysis = text(point.row, 'analysis');
            const feature = text(point.row, 'feature') || text(point.row, 'name');
            return `Analysis: ${analysis}<br>Feature: ${feature}`;
          }),
          marker: {
            color: points.map((point) => point.color),
            size: selectedKey
              ? markerSizes.map((size, index) => (keys[index] === selectedKey ? size + 6 : size))
              : markerSizes,
            opacity: markerOpacity,
            line: { color: theme.text, width: markerLineWidth },
          },
          hovertemplate: '%{text}<br>RT %{x}<br>m/z %{y}<extra></extra>',
        },
      ],
      layout: {
        uirevision: `suspects-scatter-${artifactId}`,
        autosize: true,
        margin: { l: 55, r: 20, t: 20, b: 50 },
        xaxis: {
          title: { text: 'Retention time (s)' },
          showgrid: true,
          zeroline: false,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
        },
        yaxis: {
          title: { text: 'm/z' },
          showgrid: true,
          zeroline: false,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
        },
        template: 'none',
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        hoverlabel: { bgcolor: theme.background, font: { color: theme.text } },
        hovermode: 'closest',
      },
      config: { displaylogo: false, responsive: true },
    },
    provenance: {
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.suspects_explorer',
      producer_node_id: 'suspects-explorer',
    },
    fallback: { description: 'The suspect feature scatter renderer is unavailable.' },
  };
  return (
    <VisualizationRenderer
      spec={spec}
      className="sf-feature-scatter-plotly"
      onPointClick={(point) => {
        const index =
          typeof point.pointNumber === 'number'
            ? point.pointNumber
            : typeof point.pointIndex === 'number'
              ? point.pointIndex
              : -1;
        if (index >= 0 && points[index]) onSelect(keys[index]);
      }}
      onPlotClick={onClearSelection}
      onDoubleClick={onClearSelection}
      onRelayout={onRelayout}
    />
  );
}

function evidenceSpec(
  row: Row,
  tab: EvidenceTab,
  isotope: { mz: number[]; probability: number[]; labels: string[] } | null,
  artifactId: string,
): VisualizationSpec | null {
  const theme = {
    background: cssThemeValue('--sf-surface-raised'),
    grid: cssThemeValue('--sf-border'),
    text: cssThemeValue('--sf-text'),
    experimental: cssThemeValue('--sf-accent'),
    reference: cssThemeValue('--sf-signal'),
  };
  const experimentalColor = theme.experimental;
  let data: PlotlyTrace[] = [];
  let xTitle = 'm/z';
  let yTitle = 'Intensity';
  if (tab === 'eic') {
    const x = decodeFloatArray(text(row, 'eic_rt'), text(row, 'eic_size'));
    const y = decodeFloatArray(text(row, 'eic_intensity'), text(row, 'eic_size'));
    if (!x.length || !y.length) return null;
    xTitle = 'Retention time (s)';
    data = [
      {
        type: 'scattergl',
        mode: 'lines',
        name: 'Experimental EIC',
        x,
        y,
        line: { color: experimentalColor, width: 2 },
        fill: 'tozeroy',
      },
    ];
  } else if (tab === 'ms1') {
    const x = decodeFloatArray(text(row, 'ms1_mz'), text(row, 'ms1_size'));
    const y = decodeFloatArray(text(row, 'ms1_intensity'), text(row, 'ms1_size'));
    if (!x.length || !y.length) return null;
    data = [
      {
        type: 'scattergl',
        mode: 'lines',
        name: 'Experimental MS1',
        x: x.flatMap((value) => [value, value, null]),
        y: y.flatMap((value) => [0, value, null]),
        line: { color: experimentalColor, width: 1 },
      },
    ];
    if (isotope?.mz.length) {
      const max = Math.max(...isotope.probability, 0);
      const normalized = max > 0 ? isotope.probability.map((value) => value / max) : isotope.probability;
      const scale = Math.max(...y, 0);
      data.push({
        type: 'scattergl',
        mode: 'lines',
        name: 'Expected isotope pattern',
        x: isotope.mz.flatMap((value) => [value, value, null]),
        y: normalized.flatMap((value) => [0, -value * scale, null]),
        line: { color: theme.reference, width: 1 },
      });
    }
    yTitle = 'Intensity (experimental + / isotope −)';
  } else {
    const experimentalMz = decodeFloatArray(text(row, 'ms2_mz'), text(row, 'ms2_size'));
    const experimentalIntensity = decodeFloatArray(text(row, 'ms2_intensity'), text(row, 'ms2_size'));
    const databaseMz = decodeFloatArray(text(row, 'db_ms2_mz'), text(row, 'db_ms2_size'));
    const databaseIntensity = decodeFloatArray(text(row, 'db_ms2_intensity'), text(row, 'db_ms2_size'));
    const experimentalCount = Math.min(experimentalMz.length, experimentalIntensity.length);
    const databaseCount = Math.min(databaseMz.length, databaseIntensity.length);
    if (!experimentalCount && !databaseCount) return null;
    const experimentalMax = Math.max(...experimentalIntensity.slice(0, experimentalCount), 0);
    const databaseMax = Math.max(...databaseIntensity.slice(0, databaseCount), 0);
    if (experimentalCount > 0)
      data.push({
        type: 'scatter',
        mode: 'lines',
        name: 'Experimental MS2',
        x: experimentalMz.slice(0, experimentalCount).flatMap((value) => [value, value, null]),
        y: experimentalIntensity
          .slice(0, experimentalCount)
          .flatMap((value) => [0, experimentalMax > 0 ? value / experimentalMax : 0, null]),
        line: { color: experimentalColor, width: 1 },
      });
    if (databaseCount > 0)
      data.push({
        type: 'scatter',
        mode: 'lines',
        name: 'Database MS2',
        x: databaseMz.slice(0, databaseCount).flatMap((value) => [value, value, null]),
        y: databaseIntensity
          .slice(0, databaseCount)
          .flatMap((value) => [0, databaseMax > 0 ? -value / databaseMax : 0, null]),
        line: { color: theme.reference, width: 1 },
      });
    yTitle = 'Relative intensity (experimental + / database −)';
  }
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: `suspect-evidence-${tab}-${artifactId}-${text(row, 'feature')}`,
    semantic_type: `sfms:suspect-${tab}`,
    title: tab.toUpperCase(),
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data,
      layout: {
        autosize: true,
        margin: { l: 65, r: 20, t: 35, b: 50 },
        xaxis: {
          title: { text: xTitle },
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
        },
        yaxis: {
          title: { text: yTitle },
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
          zeroline: true,
          zerolinecolor: theme.grid,
        },
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        legend: { bgcolor: theme.background },
      },
      config: { displaylogo: false, responsive: true, displayModeBar: true },
    },
    provenance: {
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.suspects_explorer',
      producer_node_id: 'suspects-explorer',
    },
    fallback: { description: 'The suspect evidence renderer is unavailable.' },
  };
}

export function SuspectsExplorer({ context }: ViewerComponentProps): ReactNode {
  const client = context.pluginApi?.client;
  const [suspects, setSuspects] = useState<Row[]>([]);
  const [totalSuspects, setTotalSuspects] = useState(0);
  const [selectedKey, setSelectedKey] = useState<string | null>(null);
  const [selectedFeatureDetail, setSelectedFeatureDetail] = useState<Row | null>(null);
  const [selectedSuspect, setSelectedSuspect] = useState<Row | null>(null);
  const [selectedSuspectLoading, setSelectedSuspectLoading] = useState(false);
  const [rightTab, setRightTab] = useState<'eic' | 'suspects'>('eic');
  const [search, setSearch] = useState('');
  const [groupBy, setGroupBy] = useState<'analysis' | 'replicate'>('replicate');
  const [numericFilters, setNumericFilters] = useState<Record<string, NumericFilter>>({});
  const initialNumericFilters = useRef<Record<string, NumericFilter>>({});
  const [columns, setColumns] = useState<Column[]>([]);
  const [booleanFilters, setBooleanFilters] = useState<Record<string, boolean>>({});
  const [textFilters, setTextFilters] = useState<Record<string, string>>({});
  const initialLoadComplete = useRef(false);
  const [isotopeOnly, setIsotopeOnly] = useState(false);
  const [showFiltered, setShowFiltered] = useState(false);
  const [, refreshTheme] = useState(0);
  const [filtersWidth, setFiltersWidth] = useState(240);
  const [detailsWidth, setDetailsWidth] = useState(380);
  const explorerRef = useRef<HTMLDivElement>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [structureCache, setStructureCache] = useState<StructureCache>({});
  const [viewport, setViewport] = useState<PlotViewport | null>(null);
  const overviewColumns = useMemo(
    () => selectSuspectOverviewColumns(context.artifact?.columns),
    [context.artifact?.columns],
  );
  const filterColumns = useMemo(() => getSuspectFilterColumns(columns, suspects), [columns, suspects]);

  const resizeFilters = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    const move = (moveEvent: PointerEvent) => {
      const bounds = explorerRef.current?.getBoundingClientRect();
      if (!bounds) return;
      const maximum = Math.max(260, bounds.width - 320 - 6 - 6 - detailsWidth);
      setFiltersWidth(Math.max(180, Math.min(maximum, moveEvent.clientX - bounds.left)));
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
      const bounds = explorerRef.current?.getBoundingClientRect();
      if (!bounds) return;
      const maximum = Math.max(280, bounds.width - filtersWidth - 320 - 6 - 6);
      setDetailsWidth(Math.max(280, Math.min(maximum, bounds.right - moveEvent.clientX)));
    };
    const stop = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', stop);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', stop, { once: true });
  };

  useEffect(() => {
    const observer = new MutationObserver(() => refreshTheme((value) => value + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    let active = true;
    if (!client || !context.artifact) {
      void Promise.resolve().then(() => {
        if (!active) return;
        setError('Suspects explorer is unavailable.');
        setLoading(false);
      });
      return () => {
        active = false;
      };
    }
    const suspectsArtifactId = context.artifact.artifact_id;
    const load = async () => {
      const suspectData = await client.artifactQuery(context.sessionId, {
        artifact_id: suspectsArtifactId,
        mode: 'sample',
        columns: overviewColumns,
        filters: overviewColumns?.includes('filtered')
          ? [{ column: 'filtered', operator: 'neq', value: 'true' }]
          : undefined,
        sampling_strategy: 'grid',
        x_bins: 512,
        y_bins: 512,
        x_column: 'exp_mass',
        y_column: 'exp_rt',
        limit: 50000,
      });
      if (!active) return;
      setSuspects(suspectData.rows.filter((row) => row && typeof row === 'object'));
      setTotalSuspects(suspectData.total_rows);
      setColumns(suspectData.columns);
      const loadedFilterColumns = getSuspectFilterColumns(suspectData.columns, suspectData.rows);
      const defaults = Object.fromEntries(
        loadedFilterColumns.numeric.map((column) => [column.name, { min: '', max: '' }]),
      );
      initialNumericFilters.current = defaults;
      setNumericFilters(defaults);
      setBooleanFilters(Object.fromEntries(loadedFilterColumns.boolean.map((column) => [column.name, false])));
      setTextFilters(Object.fromEntries(loadedFilterColumns.text.map((column) => [column.name, ''])));
      setSelectedKey(null);
      initialLoadComplete.current = true;
    };
    void load()
      .catch((reason: unknown) => {
        if (active) setError(reason instanceof Error ? reason.message : 'Could not load suspects explorer data.');
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [client, context.artifact, context.sessionId, overviewColumns]);

  useEffect(() => {
    if (!initialLoadComplete.current || !client || !context.artifact) return;
    let active = true;
    const timer = window.setTimeout(() => {
      void client
        .artifactQuery(context.sessionId, {
          artifact_id: context.artifact!.artifact_id,
          mode: 'sample',
          columns: overviewColumns,
          sampling_strategy: 'grid',
          x_bins: 512,
          y_bins: 512,
          x_column: 'exp_mass',
          y_column: 'exp_rt',
          limit: 50000,
          search: search || undefined,
          filters: [
            ...buildSuspectQueryFilters(numericFilters, booleanFilters, textFilters, isotopeOnly),
            ...(!showFiltered && overviewColumns?.includes('filtered')
              ? [{ column: 'filtered', operator: 'neq' as const, value: 'true' }]
              : []),
            ...(viewport
              ? [
                  { column: 'exp_rt', operator: 'between' as const, min: viewport.xMin, max: viewport.xMax },
                  { column: 'exp_mass', operator: 'between' as const, min: viewport.yMin, max: viewport.yMax },
                ]
              : []),
          ],
        })
        .then((response) => {
          if (!active) return;
          setSuspects(response.rows.filter((row) => row && typeof row === 'object'));
          setTotalSuspects(response.total_rows);
          setSelectedKey(null);
        })
        .catch(() => {
          // Keep the last successful overview visible while a transient query fails.
        });
    }, 250);
    return () => {
      active = false;
      window.clearTimeout(timer);
    };
  }, [
    client,
    context.artifact,
    context.sessionId,
    booleanFilters,
    isotopeOnly,
    numericFilters,
    overviewColumns,
    search,
    showFiltered,
    textFilters,
    viewport,
  ]);

  const filteredSuspects = useMemo(() => {
    const query = search.trim().toLowerCase();
    return suspects.filter((row) => {
      if (
        query &&
        !Object.values(row).some((value) =>
          String(value ?? '')
            .toLowerCase()
            .includes(query),
        )
      )
        return false;
      if (isotopeOnly && text(row, 'isotope_match').toLowerCase() !== 'true') return false;
      if (!showFiltered && text(row, 'filtered').toLowerCase() === 'true') return false;
      return (
        filterColumns.numeric.every((column) => {
          const key = column.name;
          const filter = numericFilters[key];
          const value = number(row, key);
          if (!filter || (filter.min === '' && filter.max === '')) return true;
          if (value === null) return false;
          const min = filter.min === '' ? -Infinity : Number(filter.min);
          const max = filter.max === '' ? Infinity : Number(filter.max);
          return Number.isFinite(min) && Number.isFinite(max) && value >= min && value <= max;
        }) &&
        filterColumns.boolean.every((column) => {
          const value = text(row, column.name).toLowerCase() === 'true';
          return booleanFilters[column.name] ? value : true;
        }) &&
        filterColumns.text.every((column) => {
          const filter = (textFilters[column.name] ?? '').trim().toLowerCase();
          return !filter || text(row, column.name).toLowerCase() === filter;
        })
      );
    });
  }, [booleanFilters, filterColumns, isotopeOnly, numericFilters, search, showFiltered, suspects, textFilters]);

  const featureRows = useMemo(() => {
    const unique = new Map<string, Row>();
    for (const row of filteredSuspects) {
      const key = featureKey(row);
      if (!unique.has(key)) unique.set(key, row);
    }
    return [...unique.values()];
  }, [filteredSuspects]);

  const categories = useMemo(
    () =>
      [
        ...new Set(featureRows.map((row, index) => text(row, groupBy) || text(row, 'analysis') || String(index))),
      ].sort(),
    [featureRows, groupBy],
  );
  const plotPoints = useMemo<ScatterPoint[]>(() => {
    return featureRows.flatMap((row, index) => {
      const mz = number(row, 'exp_mass');
      const rt = number(row, 'exp_rt');
      if (mz === null || rt === null) return [];
      return [
        {
          row: { ...row, mz: String(mz), rt: String(rt) },
          index,
          color: colorFor(text(row, groupBy) || text(row, 'analysis') || String(index), categories),
          intensity: number(row, 'intensity') ?? 1,
        },
      ];
    });
  }, [categories, featureRows, groupBy]);

  const selectedFeature = plotPoints.find((point) => featureKey(point.row) === selectedKey)?.row ?? null;

  useEffect(() => {
    let active = true;
    const rowKey = selectedFeature?.row_key;
    if (!selectedFeature || !rowKey || !client || !context.artifact) {
      return () => {
        active = false;
      };
    }
    void client
      .artifactQuery(context.sessionId, {
        artifact_id: context.artifact.artifact_id,
        mode: 'detail',
        row_keys: [rowKey],
        limit: 1,
      })
      .then((response) => {
        if (active) setSelectedFeatureDetail(response.rows[0] ?? null);
      })
      .catch(() => {
        if (active) setSelectedFeatureDetail(null);
      });
    return () => {
      active = false;
    };
  }, [client, context.artifact, context.sessionId, selectedFeature]);

  const openSuspectDetail = async (row: Row) => {
    setSelectedSuspect(row);
    const rowKey = row.row_key;
    const canLoadDetail = Boolean(rowKey && client && context.artifact);
    setSelectedSuspectLoading(canLoadDetail);
    if (!canLoadDetail || !client || !context.artifact || !rowKey) return;
    try {
      const detail = await client.artifactQuery(context.sessionId, {
        artifact_id: context.artifact.artifact_id,
        mode: 'detail',
        row_keys: [rowKey],
        limit: 1,
      });
      if (detail.rows[0]) setSelectedSuspect(detail.rows[0]);
    } catch {
      // Keep the projected suspect row visible when detail retrieval fails.
    } finally {
      setSelectedSuspectLoading(false);
    }
  };
  const selectedSuspects = useMemo(() => {
    if (!selectedFeature) return [];
    return filteredSuspects.filter((row) => featureKey(row) === selectedKey);
  }, [filteredSuspects, selectedFeature, selectedKey]);

  useEffect(() => {
    if (!selectedSuspect) return undefined;
    const close = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      event.stopPropagation();
      setSelectedSuspect(null);
    };
    document.addEventListener('keydown', close, true);
    return () => document.removeEventListener('keydown', close, true);
  }, [selectedSuspect]);

  if (loading) return <LoadingLogo label="Loading suspects explorer…" />;
  if (error)
    return (
      <div className="sf-suspects-explorer-state" role="alert">
        {error}
      </div>
    );
  if (!client) return <div className="sf-suspects-explorer-state">Suspects explorer is unavailable.</div>;

  return (
    <div className="sf-suspects-explorer">
      <div
        ref={explorerRef}
        className="sf-suspects-explorer-workspace"
        style={
          {
            '--sf-suspects-filters-width': `${filtersWidth}px`,
            '--sf-suspects-details-width': `${detailsWidth}px`,
          } as CSSProperties
        }
      >
        <aside className="sf-suspects-explorer-filter-panel" aria-label="Suspect filters">
          <div className="sf-suspects-explorer-panel-heading">
            <strong>Filters</strong>
            <span>
              {filteredSuspects.length} / {totalSuspects}
            </span>
          </div>
          <label className="sf-suspects-explorer-filter-label">
            Search
            <input
              value={search}
              onChange={(event) => setSearch(event.target.value)}
              placeholder="Name, formula, key…"
            />
          </label>
          <label className="sf-suspects-explorer-filter-label">
            Color by
            <select value={groupBy} onChange={(event) => setGroupBy(event.target.value as 'analysis' | 'replicate')}>
              <option value="analysis">Analysis</option>
              <option value="replicate">Replicate</option>
            </select>
          </label>
          <label className="sf-suspects-explorer-filter-checkbox">
            Isotope match only
            <input type="checkbox" checked={isotopeOnly} onChange={(event) => setIsotopeOnly(event.target.checked)} />
          </label>
          <label className="sf-suspects-explorer-filter-checkbox">
            Show filtered features
            <input type="checkbox" checked={showFiltered} onChange={(event) => setShowFiltered(event.target.checked)} />
          </label>
          <div className="sf-suspects-explorer-filter-list">
            {filterColumns.numeric.map((column) => {
              const key = column.name;
              const label = filterLabel(key);
              const filter = numericFilters[key] ?? { min: '', max: '' };
              return (
                <fieldset key={key}>
                  <legend>{label}</legend>
                  <input
                    aria-label={`${label} minimum`}
                    inputMode="decimal"
                    placeholder="min"
                    value={filter.min}
                    onChange={(event) =>
                      setNumericFilters((current) => ({ ...current, [key]: { ...filter, min: event.target.value } }))
                    }
                  />
                  <input
                    aria-label={`${label} maximum`}
                    inputMode="decimal"
                    placeholder="max"
                    value={filter.max}
                    onChange={(event) =>
                      setNumericFilters((current) => ({ ...current, [key]: { ...filter, max: event.target.value } }))
                    }
                  />
                </fieldset>
              );
            })}
            {filterColumns.boolean
              .filter((column) => column.name !== 'filtered' && column.name !== 'isotope_match')
              .map((column) => (
                <label className="sf-suspects-explorer-filter-checkbox" key={column.name}>
                  {filterLabel(column.name)}
                  <input
                    type="checkbox"
                    checked={Boolean(booleanFilters[column.name])}
                    onChange={(event) =>
                      setBooleanFilters((current) => ({ ...current, [column.name]: event.target.checked }))
                    }
                  />
                </label>
              ))}
            {filterColumns.text.map((column) => (
              <label key={column.name}>
                {filterLabel(column.name)}
                <select
                  aria-label={`${filterLabel(column.name)} value`}
                  value={textFilters[column.name] ?? ''}
                  onChange={(event) => setTextFilters((current) => ({ ...current, [column.name]: event.target.value }))}
                >
                  <option value="">Any</option>
                  {getDistinctValues(suspects, column.name).map((value) => (
                    <option key={value} value={value}>
                      {value}
                    </option>
                  ))}
                </select>
              </label>
            ))}
          </div>
        </aside>
        <div
          className="sf-suspects-explorer-splitter sf-suspects-explorer-filters-splitter"
          role="separator"
          aria-label="Resize suspect filters panel"
          aria-orientation="vertical"
          onPointerDown={resizeFilters}
        />
        <main className="sf-suspects-explorer-scatter-panel" aria-label="Feature scatter plot">
          <SuspectScatter
            artifactId={context.artifact?.artifact_id ?? context.artifactId}
            points={plotPoints}
            selectedKey={selectedKey}
            onSelect={setSelectedKey}
            onClearSelection={() => setSelectedKey(null)}
            onRelayout={(event) => {
              if (event['xaxis.autorange'] || event['yaxis.autorange']) {
                setViewport(null);
                return;
              }
              const x0 = Number(event['xaxis.range[0]']);
              const x1 = Number(event['xaxis.range[1]']);
              const y0 = Number(event['yaxis.range[0]']);
              const y1 = Number(event['yaxis.range[1]']);
              if ([x0, x1, y0, y1].every(Number.isFinite))
                setViewport({
                  xMin: Math.min(x0, x1),
                  xMax: Math.max(x0, x1),
                  yMin: Math.min(y0, y1),
                  yMax: Math.max(y0, y1),
                });
            }}
          />
        </main>
        <div
          className="sf-suspects-explorer-splitter sf-suspects-explorer-details-splitter"
          role="separator"
          aria-label="Resize suspects panel"
          aria-orientation="vertical"
          onPointerDown={resizeDetails}
        />
        <aside className="sf-suspects-explorer-suspect-panel" aria-label="Suspects for selected feature">
          <div className="sf-suspects-explorer-tabs" role="tablist" aria-label="Selected feature views">
            {(['eic', 'suspects'] as const).map((tab) => (
              <button
                type="button"
                role="tab"
                aria-selected={rightTab === tab}
                className={rightTab === tab ? 'active' : undefined}
                key={tab}
                onClick={() => setRightTab(tab)}
              >
                {tab === 'eic' ? 'EIC' : 'Suspects'}
              </button>
            ))}
          </div>
          {selectedFeature && rightTab === 'eic' ? (
            <div className="sf-suspects-explorer-right-plot">
              {selectedFeatureDetail?.row_key !== selectedFeature.row_key ? (
                <LoadingLogo label="Loading EIC…" />
              ) : selectedFeatureDetail ? (
                (() => {
                  const spec = evidenceSpec(
                    selectedFeatureDetail,
                    'eic',
                    null,
                    context.artifact?.artifact_id ?? context.artifactId,
                  );
                  return spec ? (
                    <VisualizationRenderer spec={spec} className="sf-suspects-explorer-evidence-plot" />
                  ) : (
                    <p className="sf-suspects-explorer-empty">No EIC data is available for this feature.</p>
                  );
                })()
              ) : (
                <LoadingLogo label="Loading EIC…" />
              )}
            </div>
          ) : selectedFeature ? (
            <>
              <div className="sf-suspects-explorer-selected-feature">
                <strong>{text(selectedFeature, 'feature') || 'Selected feature'}</strong>
                <span>{text(selectedFeature, 'analysis')}</span>
                <small>
                  m/z {formatValue(text(selectedFeature, 'exp_mass'))} · RT{' '}
                  {formatValue(text(selectedFeature, 'exp_rt'))}
                </small>
              </div>
              {selectedSuspects.length ? (
                <div className="sf-suspects-explorer-list">
                  {selectedSuspects.map((row, index) => (
                    <button
                      type="button"
                      className="sf-suspects-explorer-row"
                      key={`${text(row, 'name')}-${index}`}
                      onClick={() => void openSuspectDetail(row)}
                    >
                      <span>
                        <strong>{text(row, 'name') || 'Unnamed suspect'}</strong>
                        <small>{text(row, 'formula') || 'Formula unavailable'}</small>
                      </span>
                      <span className="sf-suspects-explorer-level">Level {text(row, 'id_level') || '4'}</span>
                    </button>
                  ))}
                </div>
              ) : (
                <p className="sf-suspects-explorer-empty">No suspect assignments for this feature.</p>
              )}
            </>
          ) : (
            <p className="sf-suspects-explorer-empty">Select a point to list its suspects.</p>
          )}
        </aside>
      </div>
      {selectedSuspect
        ? createPortal(
            <div
              className="sf-suspects-explorer-modal-backdrop"
              onMouseDown={(event) => {
                if (event.target === event.currentTarget) setSelectedSuspect(null);
              }}
            >
              <div
                className="sf-suspects-explorer-modal"
                role="dialog"
                aria-modal="true"
                aria-label={`${text(selectedSuspect, 'name') || 'Suspect'} identification overview`}
              >
                <header>
                  <strong>{text(selectedSuspect, 'name') || 'Suspect identification'}</strong>
                  <button
                    type="button"
                    className="sf-icon-button sf-suspects-explorer-modal-close"
                    aria-label="Close suspect identification"
                    title="Close"
                    onClick={() => setSelectedSuspect(null)}
                  >
                    <i className="fa-solid fa-xmark" aria-hidden="true" />
                  </button>
                </header>
                <div className="sf-suspects-explorer-modal-body">
                  {selectedSuspectLoading ? (
                    <LoadingLogo label="Loading suspect evidence…" />
                  ) : (
                    <>
                      <IdentificationOverview
                        row={selectedSuspect}
                        client={client}
                        cache={structureCache}
                        onRendered={(key, structure) =>
                          setStructureCache((current) => (current[key] ? current : { ...current, [key]: structure }))
                        }
                      />
                      <EvidencePlots
                        row={selectedSuspect}
                        client={client}
                        artifactId={context.artifact?.artifact_id ?? context.artifactId}
                      />
                    </>
                  )}
                </div>
              </div>
            </div>,
            document.body,
          )
        : null}
    </div>
  );
}

function buildSuspectQueryFilters(
  numericFilters: Record<string, NumericFilter>,
  booleanFilters: Record<string, boolean>,
  textFilters: Record<string, string>,
  isotopeOnly: boolean,
): ArtifactQueryFilter[] {
  const filters: ArtifactQueryFilter[] = [];
  Object.entries(numericFilters).forEach(([key, value]) => {
    if (!value || (value.min === '' && value.max === '')) return;
    if (value.min !== '' && value.max !== '')
      filters.push({ column: key, operator: 'between', min: Number(value.min), max: Number(value.max) });
    else if (value.min !== '') filters.push({ column: key, operator: 'gte', value: Number(value.min) });
    else filters.push({ column: key, operator: 'lte', value: Number(value.max) });
  });
  Object.entries(booleanFilters).forEach(([key, enabled]) => {
    if (enabled) filters.push({ column: key, operator: 'eq', value: true });
  });
  Object.entries(textFilters).forEach(([key, value]) => {
    if (value.trim()) filters.push({ column: key, operator: 'eq', value: value.trim() });
  });
  if (isotopeOnly) filters.push({ column: 'isotope_match', operator: 'eq', value: true });
  return filters;
}

function getSuspectFilterColumns(
  columns: Column[],
  rows: Row[],
): {
  numeric: Column[];
  boolean: Column[];
  text: Column[];
} {
  const knownColumns = columns.length
    ? columns
    : Array.from(new Set(rows.flatMap((row) => Object.keys(row)))).map((name): Column => ({ name }));
  const numeric: Column[] = [];
  const boolean: Column[] = [];
  const textColumns: Column[] = [];
  for (const column of knownColumns) {
    const type = (column.type ?? '').toLowerCase();
    const values = rows
      .map((row) => row[column.name])
      .filter((value): value is string => value !== null && value !== undefined);
    const inferredNumeric = values.length > 0 && values.every((value) => Number.isFinite(Number(value)));
    const inferredBoolean = values.length > 0 && values.every((value) => /^(true|false)$/i.test(value));
    if (/int|decimal|numeric|real|double|float|hugeint/.test(type) || (!type && inferredNumeric)) {
      numeric.push(column);
    } else if (type === 'boolean' || type === 'bool' || type === 'logical' || (!type && inferredBoolean)) {
      boolean.push(column);
    } else if (/char|string|text|varchar|uuid/.test(type) || (!type && values.length > 0)) {
      const distinct = new Set(values.map((value) => value.trim()).filter(Boolean));
      if (distinct.size > 0 && distinct.size <= 64 && isSuspectCategoricalFilterColumn(column.name)) {
        textColumns.push(column);
      }
    }
  }
  return { numeric, boolean, text: textColumns };
}

function isSuspectCategoricalFilterColumn(name: string): boolean {
  return !/^(formula|smiles|inchi|inchikey|name|feature|db_ms2_|eic_|ms1_|ms2_|fragments?_|raw_)/i.test(name);
}

function getDistinctValues(rows: Row[], column: string): string[] {
  return Array.from(new Set(rows.map((row) => text(row, column)).filter(Boolean)))
    .sort((left, right) => left.localeCompare(right))
    .slice(0, 64);
}

function filterLabel(key: string): string {
  const known = filterDefinitions.find(([name]) => name === key)?.[1];
  if (known) return known;
  return key.replaceAll('_', ' ').replace(/\b\w/g, (character) => character.toUpperCase());
}

function selectSuspectOverviewColumns(available: Column[] | undefined): string[] | undefined {
  if (!available?.length) return undefined;
  const scalarColumns = available
    .filter(
      (column) =>
        !/array|list|map|struct|blob|binary|json/i.test(column.type ?? '') &&
        !/^(eic_(rt|intensity)|ms1_(mz|intensity)|ms2_(mz|intensity)|db_ms2_(mz|intensity))$/i.test(column.name),
    )
    .map((column) => column.name);
  const preferred = suspectOverviewColumns.filter((column) => scalarColumns.includes(column));
  return Array.from(new Set([...preferred, ...scalarColumns]));
}
