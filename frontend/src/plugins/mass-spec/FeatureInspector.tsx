import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type CSSProperties,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from 'react';
import * as d3 from 'd3';
import logo from '../../assets/streamfind.png';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';

import type { VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';

type FeatureRow = Record<string, string | null>;
type QueryKind = 'eic' | 'ms1' | 'ms2';
type DetailTab = 'details' | QueryKind | 'network';
type SelectionMode = 'feature' | 'feature_group' | 'feature_component' | 'feature_group_component';
type FeatureColumn = { name: string; type: string };
type NumericFilter = { min: string; max: string };
type Point = { row: FeatureRow; index: number; color: string; intensity: number };

export function FeatureInspector({ context }: ViewerComponentProps): ReactNode {
  const [rows, setRows] = useState<FeatureRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [columns, setColumns] = useState<FeatureColumn[]>([]);
  const [search, setSearch] = useState('');
  const [groupBy, setGroupBy] = useState('replicate');
  const [selectBy, setSelectBy] = useState<SelectionMode>('feature');
  const [numericFilters, setNumericFilters] = useState<Record<string, NumericFilter>>({});
  const [booleanFilters, setBooleanFilters] = useState<Record<string, boolean>>({});
  const [selectedKey, setSelectedKey] = useState<string | null>(null);
  const [activeTab, setActiveTab] = useState<DetailTab>('eic');
  const [darkMode, setDarkMode] = useState(() => document.documentElement.dataset.theme === 'dark');
  const inspectorRef = useRef<HTMLDivElement>(null);
  const [filtersWidth, setFiltersWidth] = useState(240);
  const [detailsWidth, setDetailsWidth] = useState(380);

  const resizeFilters = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    const move = (moveEvent: PointerEvent) => {
      const bounds = inspectorRef.current?.getBoundingClientRect();
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
      const bounds = inspectorRef.current?.getBoundingClientRect();
      if (!bounds) return;
      const maximum = Math.max(320, bounds.width - 320 - 6 - 320);
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
    const observer = new MutationObserver(() => setDarkMode(document.documentElement.dataset.theme === 'dark'));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  const filterColumns = useMemo(() => getFilterColumns(columns, rows), [columns, rows]);

  useEffect(() => {
    let active = true;
    const api = context.pluginApi?.client;
    if (!api || !context.artifact) {
      void Promise.resolve().then(() => {
        if (!active) return;
        setLoading(false);
        setLoadError('Feature data is unavailable.');
      });
      return () => {
        active = false;
      };
    }
    const artifactId = context.artifact.artifact_id;
    const loadRows = async () => {
      const pageSize = 1000;
      const firstPage = await api.artifactData(context.sessionId, {
        artifact_id: artifactId,
        offset: 0,
        limit: pageSize,
      });
      const loadedRows = [...firstPage.rows];
      let offset = firstPage.rows.length;
      while (offset < firstPage.total_rows) {
        const page = await api.artifactData(context.sessionId, {
          artifact_id: artifactId,
          offset,
          limit: pageSize,
        });
        loadedRows.push(...page.rows);
        if (!page.rows.length) break;
        offset += page.rows.length;
      }
      setColumns(firstPage.columns);
      const loadedFilterColumns = getFilterColumns(firstPage.columns, loadedRows);
      setNumericFilters(createInitialNumericFilters(loadedFilterColumns.numeric, loadedRows));
      setBooleanFilters(Object.fromEntries(loadedFilterColumns.boolean.map((column) => [column.name, false])));
      return loadedRows;
    };
    void loadRows()
      .then((loadedRows) => {
        if (!active) return;
        setRows(loadedRows);
        setSelectedKey(null);
      })
      .catch((error: unknown) => {
        if (!active) return;
        setLoadError(error instanceof Error ? error.message : 'Feature data could not be loaded.');
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [context.artifact, context.pluginApi, context.sessionId]);

  const filteredRows = useMemo(() => {
    let searchPattern: RegExp | null = null;
    if (search) {
      try {
        searchPattern = new RegExp(search, 'i');
      } catch {
        searchPattern = null;
      }
    }
    return rows.filter((row) => {
      if (
        (selectBy === 'feature_group' && !hasSelectionValue(row, 'feature_group')) ||
        (selectBy === 'feature_component' && !hasSelectionValue(row, 'feature_component')) ||
        (selectBy === 'feature_group_component' &&
          (!hasSelectionValue(row, 'feature_group') || !hasSelectionValue(row, 'feature_component')))
      )
        return false;
      if (searchPattern && !matchesFeatureSearch(row, searchPattern)) return false;
      if (search && !searchPattern && !matchesFeatureSearch(row, new RegExp(escapeRegExp(search), 'i'))) return false;
      return (
        filterColumns.numeric.every((column) =>
          withinRange(
            row,
            [column.name],
            numericFilters[column.name]?.min ?? '',
            numericFilters[column.name]?.max ?? '',
          ),
        ) &&
        filterColumns.boolean.every((column) => booleanFilters[column.name] || booleanValue(row[column.name]) !== true)
      );
    });
  }, [booleanFilters, filterColumns, numericFilters, rows, search, selectBy]);
  const points = useMemo(() => makePoints(filteredRows, groupBy), [filteredRows, groupBy]);
  const selectedRows = useMemo(
    () => filteredRows.filter((row) => selectionKey(row, selectBy) === selectedKey),
    [filteredRows, selectBy, selectedKey],
  );

  const selectPoint = (key: string) => {
    setSelectedKey(key);
  };

  if (loading)
    return (
      <div className="sf-feature-loading" role="status" aria-live="polite">
        <img src={logo} alt="streamfind" />
        <span>Loading feature data…</span>
      </div>
    );
  if (loadError) return <div role="alert">{loadError}</div>;
  if (!rows.length) return <div role="status">No feature data is available in this artifact.</div>;

  return (
    <div
      ref={inspectorRef}
      className="sf-feature-inspector"
      style={
        {
          '--sf-feature-filters-width': `${filtersWidth}px`,
          '--sf-feature-details-width': `${detailsWidth}px`,
        } as CSSProperties
      }
    >
      <aside className="sf-feature-inspector-filters" aria-label="Feature filters">
        <header>
          <strong>
            Features {filteredRows.length} / {rows.length}
          </strong>
        </header>
        <label>
          Group by
          <select value={groupBy} onChange={(event) => setGroupBy(event.target.value)}>
            <option value="analysis">Analysis</option>
            <option value="replicate">Replicate</option>
          </select>
        </label>
        <label>
          Select by
          <select
            value={selectBy}
            onChange={(event) => {
              const nextMode = event.target.value as SelectionMode;
              setSelectBy(nextMode);
              setSelectedKey(null);
            }}
          >
            <option value="feature">Feature</option>
            <option value="feature_group">Feature group</option>
            <option value="feature_component">Feature component</option>
            <option value="feature_group_component">Group + component</option>
          </select>
        </label>
        <label>
          Search
          <input
            value={search}
            onChange={(event) => setSearch(event.target.value)}
            placeholder="Filter features (regex)"
          />
        </label>
        <div className="sf-feature-filter-list" aria-label="Feature column filters">
          {filterColumns.numeric.map((column) => {
            const filter = numericFilters[column.name] ?? { min: '', max: '' };
            return (
              <fieldset key={column.name}>
                <legend>{column.name}</legend>
                <input
                  aria-label={`${column.name} minimum`}
                  inputMode="decimal"
                  value={filter.min}
                  onChange={(event) =>
                    setNumericFilters((current) => ({
                      ...current,
                      [column.name]: { ...filter, min: event.target.value },
                    }))
                  }
                  placeholder="min"
                />
                <input
                  aria-label={`${column.name} maximum`}
                  inputMode="decimal"
                  value={filter.max}
                  onChange={(event) =>
                    setNumericFilters((current) => ({
                      ...current,
                      [column.name]: { ...filter, max: event.target.value },
                    }))
                  }
                  placeholder="max"
                />
              </fieldset>
            );
          })}
          {filterColumns.boolean.map((column) => (
            <label className="sf-feature-filter-checkbox" key={column.name}>
              {column.name}
              <input
                type="checkbox"
                checked={Boolean(booleanFilters[column.name])}
                onChange={(event) =>
                  setBooleanFilters((current) => ({ ...current, [column.name]: event.target.checked }))
                }
              />
            </label>
          ))}
        </div>
      </aside>
      <div
        className="sf-feature-inspector-splitter sf-feature-inspector-filters-splitter"
        role="separator"
        aria-label="Resize feature filters panel"
        aria-orientation="vertical"
        onPointerDown={resizeFilters}
      />
      <main className="sf-feature-inspector-plot" aria-label="Feature scatter plot">
        <header>
          <strong>Feature map</strong>
          <small>retention time × m/z</small>
        </header>
        <FeaturePlot
          artifactId={context.artifact?.artifact_id ?? context.artifactId}
          points={points}
          selectBy={selectBy}
          selectedKey={selectedKey}
          darkMode={darkMode}
          onSelect={selectPoint}
          onClearSelection={() => setSelectedKey(null)}
        />
      </main>
      <div
        className="sf-feature-inspector-splitter sf-feature-inspector-details-splitter"
        role="separator"
        aria-label="Resize feature details panel"
        aria-orientation="vertical"
        onPointerDown={resizeDetails}
      />
      <section className="sf-feature-inspector-details" aria-label="Feature details">
        <nav role="tablist" aria-label="Feature detail views">
          {(['eic', 'ms1', 'ms2', 'network', 'details'] as DetailTab[]).map((tab) => (
            <button
              type="button"
              role="tab"
              aria-selected={activeTab === tab}
              className={activeTab === tab ? 'active' : undefined}
              key={tab}
              onClick={() => setActiveTab(tab)}
            >
              {tab === 'details' ? 'Details' : tab.toUpperCase()}
            </button>
          ))}
        </nav>
        {!selectedRows.length ? (
          <div className="sf-feature-inspector-empty">Select a point in the scatter plot.</div>
        ) : null}
        {selectedRows.length && activeTab === 'details' ? <FeatureDetails rows={selectedRows} /> : null}
        {selectedRows.length && (activeTab === 'eic' || activeTab === 'ms1' || activeTab === 'ms2') ? (
          <FeatureSignalPlot
            kind={activeTab}
            rows={selectedRows}
            artifactId={context.artifact?.artifact_id ?? context.artifactId}
            darkMode={darkMode}
          />
        ) : null}
        {selectedRows.length && activeTab === 'network' ? (
          <FeatureNetwork rows={rows} selectedKey={selectedKey} selectBy={selectBy} darkMode={darkMode} />
        ) : null}
      </section>
    </div>
  );
}

function FeaturePlot({
  artifactId,
  points,
  selectBy,
  selectedKey,
  darkMode,
  onSelect,
  onClearSelection,
}: {
  artifactId: string;
  points: Point[];
  selectBy: SelectionMode;
  selectedKey: string | null;
  darkMode: boolean;
  onSelect: (key: string) => void;
  onClearSelection: () => void;
}) {
  return (
    <VisualizationRenderer
      spec={featureScatterSpec(artifactId, points, selectBy, selectedKey, darkMode)}
      className="sf-feature-scatter-plotly"
      onPointClick={(point) => {
        // Plotly's pointIndex is the authoritative identity of the rendered row.
        // Do not depend on customdata surviving the Plotly event boundary unchanged:
        // component selection must use the clicked dot's analysis and component.
        const pointIndex =
          typeof point.pointNumber === 'number'
            ? point.pointNumber
            : typeof point.pointIndex === 'number'
              ? point.pointIndex
              : undefined;
        if (pointIndex !== undefined && points[pointIndex]) {
          onSelect(selectionKey(points[pointIndex].row, selectBy));
        } else if (typeof point.customdata === 'string') {
          onSelect(point.customdata);
        }
      }}
      onPlotClick={onClearSelection}
      onDoubleClick={onClearSelection}
    />
  );
}

function featureScatterSpec(
  artifactId: string,
  points: Point[],
  selectBy: SelectionMode,
  selectedKey: string | null,
  darkMode: boolean,
): VisualizationSpec {
  const plotTheme = getPlotTheme(darkMode);
  const intensities = points.map((point) => point.intensity);
  const intensityMin = Math.min(...intensities);
  const intensityMax = Math.max(...intensities);
  const markerSizes = points.map((point) =>
    intensityMax > intensityMin ? 10 + ((point.intensity - intensityMin) / (intensityMax - intensityMin)) * 20 : 20,
  );
  const selectedKeys = points.map((point) => selectionKey(point.row, selectBy));
  const markerOpacity = selectedKey ? selectedKeys.map((key) => (key === selectedKey ? 1 : 0.2)) : 1;
  const markerLineWidth = selectedKey ? selectedKeys.map((key) => (key === selectedKey ? 2 : 0.5)) : 0.5;
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: `feature-scatter-${artifactId}`,
    semantic_type: 'sfms:featureScatter',
    title: 'NTA feature map',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: [
        {
          type: 'scattergl',
          mode: 'markers',
          x: points.map((point) => numericValue(point.row, ['rt', 'retention_time']) ?? 0),
          y: points.map((point) => numericValue(point.row, ['mz', 'feature_mz']) ?? 0),
          customdata: points.map((point) => selectionKey(point.row, selectBy)),
          marker: {
            color: points.map((point) => point.color),
            size: selectedKey
              ? markerSizes.map((size, index) => (selectedKeys[index] === selectedKey ? size + 6 : size))
              : markerSizes,
            opacity: markerOpacity,
            line: { color: plotTheme.text, width: markerLineWidth },
          },
          text: points.map((point) => point.row.feature_id ?? point.row.feature ?? point.row.id ?? ''),
          hovertemplate: '%{text}<br>RT %{x}<br>m/z %{y}<extra></extra>',
        },
      ],
      layout: {
        margin: { l: 50, r: 30, t: 30, b: 50 },
        xaxis: {
          title: { text: 'Retention time (s)' },
          showgrid: true,
          zeroline: false,
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          tickfont: { color: plotTheme.text },
          titlefont: { color: plotTheme.text },
        },
        yaxis: {
          title: { text: 'm/z' },
          showgrid: true,
          zeroline: false,
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          tickfont: { color: plotTheme.text },
          titlefont: { color: plotTheme.text },
        },
        template: 'none',
        paper_bgcolor: plotTheme.background,
        plot_bgcolor: plotTheme.background,
        font: { color: plotTheme.text },
        hoverlabel: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
        hovermode: 'closest',
      },
      config: { displaylogo: false, responsive: true },
    },
    provenance: {
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.feature_inspector',
      producer_node_id: 'feature-inspector',
    },
    fallback: { description: 'The feature scatter renderer is unavailable.' },
  };
}

function FeatureDetails({ rows }: { rows: FeatureRow[] }) {
  const fields = Array.from(new Set(rows.flatMap((row) => Object.keys(row))));
  return (
    <div className="sf-feature-details">
      <strong>{rows.length > 1 ? `${rows.length} selected features` : 'Selected feature'}</strong>
      <div className="sf-feature-details-table-wrap">
        <table className="sf-feature-details-table">
          <thead>
            <tr>
              <th scope="col">Property</th>
              {rows.map((row, index) => (
                <th scope="col" key={`${String(row.feature ?? row.feature_id ?? index)}-${index}`}>
                  {String(row.feature ?? row.feature_id ?? `Feature ${index + 1}`)}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {fields.map((field) => (
              <tr key={field}>
                <th scope="row">{field}</th>
                {rows.map((row, index) => (
                  <td key={`${field}-${index}`}>{row[field] ?? '—'}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

function FeatureNetwork({
  rows,
  selectedKey,
  selectBy,
  darkMode,
}: {
  rows: FeatureRow[];
  selectedKey: string | null;
  selectBy: SelectionMode;
  darkMode: boolean;
}) {
  const model = useMemo(() => buildFeatureNetwork(rows, selectedKey, selectBy), [rows, selectedKey, selectBy]);
  if (!model.nodes.length)
    return <div className="sf-feature-inspector-empty">No network relationships are available for this selection.</div>;
  return <D3FeatureNetwork model={model} darkMode={darkMode} />;
}

type FeatureNetworkModel = {
  nodes: Array<{
    id: string;
    label: string;
    tag: string;
    details: string;
    annotation: string;
    tagKind: 'main' | 'isotope' | 'loss' | 'adduct';
    x: number;
    y: number;
  }>;
  edges: Array<{ source: string; target: string; weight: number }>;
};

function networkTagKind(row: FeatureRow): FeatureNetworkModel['nodes'][number]['tagKind'] {
  const adduct = String(row.adduct ?? '').toLowerCase();
  const annotation = [row.annotation_type, row.annotation_category, row.annotation_element]
    .filter(Boolean)
    .join(' ')
    .toLowerCase();
  if (adduct.includes('loss') || annotation.includes('loss')) return 'loss';
  if (adduct.includes('isotope') || annotation.includes('isotope') || /m\s*\+\s*\d/.test(adduct)) return 'isotope';
  if (
    adduct.includes('m+h') ||
    adduct.includes('m-h') ||
    annotation.includes('m+h') ||
    annotation.includes('m-h') ||
    !adduct
  )
    return 'main';
  return 'adduct';
}

function networkTagLabel(row: FeatureRow, tagKind: FeatureNetworkModel['nodes'][number]['tagKind']): string {
  const adduct = String(row.adduct ?? '').trim();
  if (tagKind === 'isotope') {
    return String(row.annotation_element ?? row.isotope_element ?? row.isotope ?? adduct)
      .replace(/^isotope\s*/i, '')
      .trim();
  }
  if (tagKind === 'loss') {
    return String(row.annotation_element ?? row.loss_element ?? adduct)
      .replace(/^\s*(?:neutral\s+)?loss\s*/i, '')
      .replace(/^\[|\]$/g, '')
      .trim();
  }
  return (adduct || String(row.annotation_element ?? row.annotation_type ?? row.feature ?? row.feature_id ?? 'feature'))
    .replace(/\badduct\b\s*/gi, '')
    .trim();
}

// eslint-disable-next-line react-refresh/only-export-components
export function buildFeatureNetwork(
  rows: FeatureRow[],
  selectedKey: string | null,
  selectBy: SelectionMode,
): FeatureNetworkModel {
  const selected = rows.filter((row) => selectionKey(row, selectBy) === selectedKey);
  const selectedAnalysis = selected[0]?.analysis ?? null;
  const sameAnalysis = (row: FeatureRow) => String(row.analysis ?? '') === String(selectedAnalysis ?? '');
  const components = new Set(selected.map((row) => row.feature_component).filter(Boolean));
  const groups = new Set(selected.map((row) => row.feature_group).filter(Boolean));
  const featureNodeId = (row: FeatureRow, index = 0) =>
    `${String(row.analysis ?? '')}\u0000${String(row.feature ?? row.feature_id ?? index)}`;
  const members = rows.filter((row) => {
    if (selectBy === 'feature_component')
      return sameAnalysis(row) && Boolean(row.feature_component && components.has(row.feature_component));
    if (selectBy === 'feature_group') return Boolean(row.feature_group && groups.has(row.feature_group));
    if (selectBy === 'feature_group_component')
      return selected.some(
        (item) =>
          item.feature_group &&
          item.feature_component &&
          item.feature_group === row.feature_group &&
          item.feature_component === row.feature_component,
      );
    if (selected[0]?.feature_component)
      return (
        sameAnalysis(row) && Boolean(row.feature_component && row.feature_component === selected[0].feature_component)
      );
    if (selected[0]?.feature_group)
      return sameAnalysis(row) && Boolean(row.feature_group && row.feature_group === selected[0].feature_group);
    return selected.some(
      (item) => String(item.analysis ?? '') === String(row.analysis ?? '') && item.feature === row.feature,
    );
  });
  const ids = new Set(members.map((row) => featureNodeId(row)).filter(Boolean));
  const nodes = members.map((row, index) => ({
    id: featureNodeId(row, index),
    label: String(row.feature ?? row.feature_id ?? index),
    tag: networkTagLabel(row, networkTagKind(row)),
    details: [
      `Feature: ${String(row.feature ?? row.feature_id ?? index)}`,
      `Tag: ${networkTagLabel(row, networkTagKind(row))}`,
      `Analysis: ${row.analysis ?? '—'}`,
      `m/z: ${row.mz ?? '—'}`,
      `RT: ${row.rt ?? '—'}`,
      `Intensity: ${row.intensity ?? row.max_intensity ?? '—'}`,
      `Group: ${row.feature_group ?? '—'}`,
      `Component: ${row.feature_component ?? '—'}`,
    ].join('\n'),
    x: Number(row.mz ?? 0),
    y: Number(row.rt ?? 0),
    annotation: [row.name, row.annotation_type, row.annotation_category, row.annotation_element]
      .filter(Boolean)
      .join(' · '),
    tagKind: networkTagKind(row),
  }));
  const rowById = new Map(members.map((row, index) => [featureNodeId(row, index), row]));
  const persistedEdges = members.flatMap((row) => {
    const source = featureNodeId(row);
    const kind = networkTagKind(row);
    const targetFeature =
      kind === 'main'
        ? String(row.component_best_partner ?? '').trim()
        : String(row.annotation_parent_feature ?? '').trim();
    const targetRow = members.find(
      (candidate) =>
        String(candidate.analysis ?? '') === String(row.analysis ?? '') &&
        String(candidate.feature ?? candidate.feature_id ?? '') === targetFeature,
    );
    const target = targetRow ? featureNodeId(targetRow) : '';
    return targetFeature && source && source !== target && ids.has(target)
      ? [{ source, target, weight: Number(row.component_max_correlation ?? row.component_mean_correlation ?? 0) }]
      : [];
  });
  const linked = new Set<string>();
  const validPersistedEdges = persistedEdges.filter((edge) => {
    const source = rowById.get(edge.source);
    const target = rowById.get(edge.target);
    if (!source || !target) return false;
    const sourceKind = networkTagKind(source);
    const targetKind = networkTagKind(target);
    if (sourceKind === 'main' && targetKind === 'main') {
      const sourceGroup = typeof source.feature_group === 'string' ? source.feature_group.trim() : '';
      const targetGroup = typeof target.feature_group === 'string' ? target.feature_group.trim() : '';
      if (!sourceGroup || !targetGroup || sourceGroup !== targetGroup) return false;
    } else if (
      sourceKind === 'loss'
        ? !['main', 'loss'].includes(targetKind)
        : sourceKind === 'adduct'
          ? targetKind !== 'main'
          : sourceKind !== 'isotope' || !['main', 'loss', 'adduct'].includes(targetKind)
    ) {
      return false;
    }
    linked.add(`${edge.source}|${edge.target}`);
    return true;
  });
  const edges = validPersistedEdges;
  const selectedIds = new Set(selected.map((row) => featureNodeId(row)).filter(Boolean));
  const connectedIds = new Set(selectedIds);
  let expanded = true;
  while (expanded) {
    expanded = false;
    edges.forEach((edge) => {
      if (!connectedIds.has(edge.source) && !connectedIds.has(edge.target)) return;
      if (!connectedIds.has(edge.source)) {
        connectedIds.add(edge.source);
        expanded = true;
      }
      if (!connectedIds.has(edge.target)) {
        connectedIds.add(edge.target);
        expanded = true;
      }
    });
  }
  return {
    nodes: nodes.filter((node) => connectedIds.has(node.id)),
    edges: edges.filter((edge) => connectedIds.has(edge.source) && connectedIds.has(edge.target)),
  };
}

function D3FeatureNetwork({ model, darkMode }: { model: FeatureNetworkModel; darkMode: boolean }) {
  type NetworkNode = FeatureNetworkModel['nodes'][number] & d3.SimulationNodeDatum;
  type NetworkLink = { source: string | NetworkNode; target: string | NetworkNode; weight: number };
  const svgRef = useRef<SVGSVGElement>(null);
  const fitNetworkRef = useRef<(() => void) | null>(null);
  const zoomInRef = useRef<(() => void) | null>(null);
  const zoomOutRef = useRef<(() => void) | null>(null);
  const [zoomPercent, setZoomPercent] = useState(100);
  useEffect(() => {
    const element = svgRef.current;
    if (!element) return;
    const svg = d3.select(element);
    const width = Math.max(320, element.clientWidth || 640);
    const height = Math.max(320, element.clientHeight || 520);
    svg.selectAll('*').remove();
    const viewport = svg.append('g').attr('class', 'sf-network-viewport');
    const zoom = d3
      .zoom<SVGSVGElement, unknown>()
      .scaleExtent([0.35, 4])
      .filter((event) => event.type !== 'wheel' || (event as WheelEvent).ctrlKey)
      .wheelDelta((event) => {
        const wheelEvent = event as WheelEvent;
        const modeScale = wheelEvent.deltaMode === 1 ? 0.01 : wheelEvent.deltaMode === 2 ? 0.1 : 0.0002;
        return -wheelEvent.deltaY * modeScale;
      })
      .on('zoom', (event) => {
        viewport.attr('transform', event.transform);
        setZoomPercent(Math.round(event.transform.k * 100));
      });
    svg.call(zoom);
    zoomInRef.current = () => svg.call(zoom.scaleBy, 1.2);
    zoomOutRef.current = () => svg.call(zoom.scaleBy, 1 / 1.2);
    const handlePan = (event: WheelEvent) => {
      if (event.ctrlKey) return;
      event.preventDefault();
      const current = d3.zoomTransform(element);
      svg.call(zoom.transform, current.translate(-event.deltaX, -event.deltaY));
    };
    element.addEventListener('wheel', handlePan, { passive: false });
    const nodes: NetworkNode[] = model.nodes.map((node) => ({ ...node }));
    const links: NetworkLink[] = model.edges.map((edge) => ({ ...edge }));
    const simulation = d3
      .forceSimulation<NetworkNode>(nodes)
      .force(
        'link',
        d3
          .forceLink<NetworkNode, NetworkLink>(links)
          .id((node) => node.id)
          .distance(90),
      )
      .force('charge', d3.forceManyBody().strength(-180))
      .force('center', d3.forceCenter(width / 2, height / 2));
    const link = viewport
      .append('g')
      .attr('class', 'sf-network-links')
      .selectAll<SVGLineElement, NetworkLink>('line')
      .data(links)
      .join('line')
      .attr('stroke', 'var(--sf-border)')
      .attr('stroke-width', (edge) => Math.max(1, Math.min(6, Math.abs(edge.weight) * 5)));
    const node = viewport
      .append('g')
      .attr('class', 'sf-network-nodes')
      .selectAll<SVGGElement, NetworkNode>('g')
      .data(nodes)
      .join('g')
      .call(
        d3
          .drag<SVGGElement, NetworkNode>()
          .on('start', (event, item) => {
            if (!event.active) simulation.alphaTarget(0.3).restart();
            item.fx = item.x;
            item.fy = item.y;
          })
          .on('drag', (event, item) => {
            item.fx = event.x;
            item.fy = event.y;
          })
          .on('end', (event, item) => {
            if (!event.active) simulation.alphaTarget(0);
            item.fx = null;
            item.fy = null;
          }),
      );
    const palette = darkMode
      ? {
          main: { fill: '#145a4a', text: '#e4fff5' },
          isotope: { fill: '#4b5563', text: '#f3f4f6' },
          loss: { fill: '#6e2428', text: '#ffe4e6' },
          adduct: { fill: '#1f3f6d', text: '#e0ecff' },
        }
      : {
          main: { fill: '#2e7d5b', text: '#ffffff' },
          isotope: { fill: '#6b7280', text: '#ffffff' },
          loss: { fill: '#9f2d2d', text: '#ffffff' },
          adduct: { fill: '#1d4f91', text: '#ffffff' },
        };
    node
      .append('rect')
      .attr(
        'x',
        (item) => -((Math.max(item.tag.length, item.tagKind === 'main' ? item.label.length : 0) * 7.2 + 18) / 2),
      )
      .attr('y', (item) => (item.tagKind === 'main' ? -22 : -12))
      .attr('width', (item) => Math.max(item.tag.length, item.tagKind === 'main' ? item.label.length : 0) * 7.2 + 18)
      .attr('height', (item) => (item.tagKind === 'main' ? 44 : 24))
      .attr('rx', 7)
      .attr('fill', (item) => palette[item.tagKind].fill)
      .attr('stroke', (item) => palette[item.tagKind].text);
    const nodeText = node
      .append('text')
      .attr('text-anchor', 'middle')
      .attr('fill', (item) => palette[item.tagKind].text)
      .attr('font-size', 12);
    nodeText
      .append('tspan')
      .attr('x', 0)
      .attr('dy', (item) => (item.tagKind === 'main' ? '-0.1em' : '0.35em'))
      .text((item) => item.tag);
    nodeText
      .filter((item) => item.tagKind === 'main')
      .append('tspan')
      .attr('x', 0)
      .attr('dy', '1.15em')
      .attr('font-size', 10)
      .text((item) => item.label);
    node.append('title').text((item) => item.details);
    fitNetworkRef.current = () => {
      const positioned = nodes.filter((item) => Number.isFinite(item.x) && Number.isFinite(item.y));
      if (!positioned.length) return;
      const padding = 36;
      const xValues = positioned.map((item) => item.x ?? 0);
      const yValues = positioned.map((item) => item.y ?? 0);
      const minX = Math.min(...xValues) - 70;
      const maxX = Math.max(...xValues) + 70;
      const minY = Math.min(...yValues) - 35;
      const maxY = Math.max(...yValues) + 35;
      const scale = Math.max(
        0.35,
        Math.min(4, (width - padding * 2) / (maxX - minX || 1), (height - padding * 2) / (maxY - minY || 1)),
      );
      const tx = width / 2 - ((minX + maxX) / 2) * scale;
      const ty = height / 2 - ((minY + maxY) / 2) * scale;
      svg.transition().duration(250).call(zoom.transform, d3.zoomIdentity.translate(tx, ty).scale(scale));
    };
    simulation.on('tick', () => {
      link
        .attr('x1', (edge) => (edge.source as (typeof nodes)[number]).x ?? 0)
        .attr('y1', (edge) => (edge.source as (typeof nodes)[number]).y ?? 0)
        .attr('x2', (edge) => (edge.target as (typeof nodes)[number]).x ?? 0)
        .attr('y2', (edge) => (edge.target as (typeof nodes)[number]).y ?? 0);
      node.attr('transform', (item) => `translate(${item.x ?? 0},${item.y ?? 0})`);
    });
    return () => {
      simulation.stop();
      element.removeEventListener('wheel', handlePan);
      svg.on('.zoom', null);
    };
  }, [darkMode, model]);
  return (
    <div className="sf-feature-network-wrap">
      <div className="sf-feature-network-toolbar" onMouseDown={(event) => event.stopPropagation()}>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={() => zoomInRef.current?.()}
          title="Zoom in"
          aria-label="Zoom in"
        >
          <i className="fa-solid fa-plus" />
        </button>
        <span className="sf-canvas-zoom">{zoomPercent}%</span>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={() => zoomOutRef.current?.()}
          title="Zoom out"
          aria-label="Zoom out"
        >
          <i className="fa-solid fa-minus" />
        </button>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={() => fitNetworkRef.current?.()}
          title="Center and fit all nodes"
          aria-label="Center and fit all nodes"
        >
          <i className="fa-solid fa-crosshairs" />
        </button>
      </div>
      <svg ref={svgRef} className="sf-feature-network" role="img" aria-label="Feature network" />
    </div>
  );
}

function FeatureSignalPlot({
  kind,
  rows,
  artifactId,
  darkMode,
}: {
  kind: QueryKind;
  rows: FeatureRow[];
  artifactId: string;
  darkMode: boolean;
}) {
  const plotTheme = getPlotTheme(darkMode);
  const prefix = kind === 'eic' ? 'eic' : kind;
  const labels = rows.map((row) => String(row.feature ?? row.feature_id ?? row.name ?? 'feature'));
  const categories = Array.from(new Set(labels)).sort();
  const traces = rows.flatMap((row) => {
    const x = decodeFloatArray(row[`${prefix}_${kind === 'eic' ? 'rt' : 'mz'}`], row[`${prefix}_size`]);
    const y = decodeFloatArray(row[`${prefix}_${kind === 'eic' ? 'intensity' : 'intensity'}`], row[`${prefix}_size`]);
    if (!x.length || !y.length) return [];
    const label = String(row.feature ?? row.feature_id ?? row.name ?? 'feature');
    const color = colorFor(label, categories);
    const isSpectrum = kind === 'ms1' || kind === 'ms2';
    return [
      {
        type: 'scattergl',
        mode: isSpectrum ? 'lines' : 'lines',
        name: label,
        x: isSpectrum ? x.flatMap((value) => [value, value, null]) : x,
        y: isSpectrum ? y.flatMap((value) => [0, value, null]) : y,
        line: { color, width: isSpectrum ? 1 : 2 },
        ...(kind === 'eic' ? { fill: 'tozeroy', fillcolor: withAlpha(color, 0.4) } : {}),
        hovertemplate: buildSignalHoverTemplate(row, label, kind),
      },
      ...(isSpectrum
        ? [
            {
              type: 'scattergl',
              mode: 'markers+text',
              name: label,
              x,
              y,
              marker: { size: 4, color },
              text: x.map((value) => value.toFixed(4)),
              textposition: 'top center',
              textfont: { size: 9, color },
              hovertemplate: buildSignalHoverTemplate(row, label, kind),
              showlegend: false,
            },
          ]
        : []),
    ];
  });
  if (!traces.length)
    return (
      <div className="sf-feature-inspector-empty">
        No encoded {kind.toUpperCase()} data is available for the selected feature.
      </div>
    );
  const spec: VisualizationSpec = {
    schema: 'streamfind.visualization/v1',
    visualization_id: `${kind}-${artifactId}`,
    semantic_type: `sfms:${kind}`,
    title: kind.toUpperCase(),
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 50, r: 20, t: 30, b: 50 },
        xaxis: {
          title: { text: kind === 'eic' ? 'Retention time (s)' : 'm/z' },
          showgrid: true,
          zeroline: false,
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          tickfont: { color: plotTheme.text },
          titlefont: { color: plotTheme.text },
        },
        yaxis: {
          title: { text: 'Intensity' },
          showgrid: true,
          zeroline: false,
          gridcolor: plotTheme.grid,
          linecolor: plotTheme.text,
          tickfont: { color: plotTheme.text },
          titlefont: { color: plotTheme.text },
        },
        legend: {
          orientation: 'v',
          x: 0.99,
          y: 0.99,
          xanchor: 'right',
          yanchor: 'top',
          bgcolor: plotTheme.background,
          bordercolor: plotTheme.grid,
          borderwidth: 1,
        },
        template: 'none',
        paper_bgcolor: plotTheme.background,
        plot_bgcolor: plotTheme.background,
        font: { color: plotTheme.text },
        hoverlabel: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
      },
      config: { displaylogo: false, responsive: true },
    },
    provenance: {
      source_artifact_ids: [artifactId],
      producer_operation_id: 'frontend.feature_encoded_arrays',
      producer_node_id: 'feature-inspector',
    },
    fallback: { description: `The ${kind.toUpperCase()} renderer is unavailable.` },
  };
  return <VisualizationRenderer spec={spec} className="sf-feature-query-plot" />;
}

function buildSignalHoverTemplate(row: FeatureRow, label: string, kind: QueryKind): string {
  const fields: Array<[string, string | null | undefined]> = [
    ['Feature', label],
    ['Analysis', row.analysis],
    ['Replicate', row.replicate],
    ['Feature component', row.feature_component],
    ['Feature group', row.feature_group],
    ['m/z', row.mz ?? row.mass],
    ['Retention time', row.rt],
    ['Intensity', row.intensity],
    ['Area', row.area],
    ['Polarity', row.polarity],
    ['Adduct', row.adduct],
    ['Annotation', row.annotation_type],
    ['Annotation category', row.annotation_category],
    ['Annotation element', row.annotation_element],
    ['Formula', row.formula],
  ];
  const metadata = fields
    .filter(([, value]) => value !== null && value !== undefined && value !== '')
    .map(([name, value]) => `${escapeHoverText(name)}: ${escapeHoverText(String(value))}`);
  const axisLabels = kind === 'eic' ? ['Retention time (s)', 'Intensity'] : ['m/z', 'Intensity'];
  return `${metadata.join('<br>')}<br>${axisLabels[0]}: %{x}<br>${axisLabels[1]}: %{y}<extra></extra>`;
}

function escapeHoverText(value: string): string {
  return value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

function decodeFloatArray(encoded: string | null | undefined, size: string | null | undefined): number[] {
  if (!encoded) return [];
  try {
    const binary = atob(encoded);
    const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
    const expected = Number(size ?? 0);
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

function withAlpha(hex: string, alpha: number): string {
  const red = Number.parseInt(hex.slice(1, 3), 16);
  const green = Number.parseInt(hex.slice(3, 5), 16);
  const blue = Number.parseInt(hex.slice(5, 7), 16);
  return `rgba(${red}, ${green}, ${blue}, ${alpha})`;
}

function getFilterColumns(
  columns: FeatureColumn[],
  rows: FeatureRow[],
): { numeric: FeatureColumn[]; boolean: FeatureColumn[] } {
  const knownColumns = columns.length
    ? columns
    : Array.from(new Set(rows.flatMap((row) => Object.keys(row)))).map((name) => ({ name, type: '' }));
  const numeric: FeatureColumn[] = [];
  const boolean: FeatureColumn[] = [];
  knownColumns.forEach((column) => {
    const type = column.type.toLowerCase();
    const values = rows.map((row) => row[column.name]).filter((value): value is string => value !== null);
    const inferredNumeric = values.length > 0 && values.every((value) => Number.isFinite(Number(value)));
    const inferredBoolean = values.length > 0 && values.every((value) => /^(true|false)$/i.test(value));
    if (isNumericType(type) || (!type && inferredNumeric)) numeric.push(column);
    else if (
      (isBooleanType(type) || (!type && inferredBoolean)) &&
      !['component_is_core', 'component_bridge_flag'].includes(column.name)
    ) {
      boolean.push(column);
    }
  });
  return { numeric, boolean };
}

function isNumericType(type: string): boolean {
  return /int|decimal|numeric|real|double|float|hugeint/.test(type);
}

function isBooleanType(type: string): boolean {
  return type === 'boolean' || type === 'bool' || type === 'logical';
}

function createInitialNumericFilters(columns: FeatureColumn[], rows: FeatureRow[]): Record<string, NumericFilter> {
  return Object.fromEntries(
    columns.map((column) => {
      const values = rows
        .map((row) => numericValue(row, [column.name]))
        .filter((value): value is number => value !== undefined);
      return [
        column.name,
        {
          min: values.length ? String(Math.min(...values)) : '',
          max: values.length ? String(Math.max(...values)) : '',
        },
      ];
    }),
  );
}

function booleanValue(value: string | null | undefined): boolean | undefined {
  if (value === null || value === undefined) return undefined;
  if (/^true$/i.test(value)) return true;
  if (/^false$/i.test(value)) return false;
  return undefined;
}

function makePoints(rows: FeatureRow[], groupBy: string): Point[] {
  const categories = Array.from(
    new Set(rows.map((row, index) => String(row[groupBy] ?? row.analysis ?? index))),
  ).sort();
  return rows.flatMap((row, index) => {
    const mz = numericValue(row, ['mz', 'feature_mz']);
    const rt = numericValue(row, ['rt', 'retention_time']);
    if (mz === undefined || rt === undefined) return [];
    return [
      {
        row,
        index,
        color: colorFor(String(row[groupBy] ?? row.analysis ?? index), categories),
        intensity: numericValue(row, ['intensity', 'feature_intensity', 'max_intensity']) ?? 0,
      },
    ];
  });
}

function selectionKey(row: FeatureRow, mode: SelectionMode): string {
  if (mode === 'feature_group_component') {
    return `${String(row.feature_group ?? '')}\u0000${String(row.feature_component ?? '')}`;
  }
  const value = String(row[mode] ?? row.feature ?? row.feature_id ?? row.id ?? 'unknown');
  if (mode === 'feature_group') return value;
  return `${String(row.analysis ?? '')}\u0000${value}`;
}

function hasSelectionValue(row: FeatureRow, field: string): boolean {
  return String(row[field] ?? '').trim() !== '';
}

function matchesFeatureSearch(row: FeatureRow, pattern: RegExp): boolean {
  return Object.values(row).some((value) => {
    pattern.lastIndex = 0;
    return pattern.test(String(value ?? ''));
  });
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function withinRange(row: FeatureRow, names: string[], min: string, max: string): boolean {
  const value = numericValue(row, names);
  if (value === undefined) return false;
  return (min === '' || value >= Number(min)) && (max === '' || value <= Number(max));
}

function getPlotTheme(darkMode: boolean): { text: string; grid: string; background: string } {
  const modalBackground =
    typeof document !== 'undefined'
      ? getComputedStyle(document.documentElement).getPropertyValue('--sf-surface-raised').trim()
      : '';
  const background = modalBackground || (darkMode ? '#132226' : '#ffffff');
  return darkMode
    ? { text: '#ffffff', grid: 'rgba(255, 255, 255, 0.16)', background }
    : { text: '#000000', grid: 'rgba(0, 0, 0, 0.14)', background };
}

function numericValue(row: FeatureRow, names: string[]): number | undefined {
  const value = names.map((name) => row[name]).find((item): item is string => Boolean(item));
  if (value === undefined) return undefined;
  const number = Number(value);
  return Number.isFinite(number) ? number : undefined;
}

function colorFor(value: string, categories: string[] = [value]): string {
  const palette = [
    '#11696f',
    '#0f9f6e',
    '#a16207',
    '#465d5e',
    '#0b5056',
    '#7c3aed',
    '#c2410c',
    '#0369a1',
    '#be123c',
    '#4d7c0f',
    '#9333ea',
    '#0891b2',
  ];
  return palette[Math.max(0, categories.indexOf(value)) % palette.length];
}
