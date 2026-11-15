import { useEffect, useMemo, useRef, useState } from 'react';
import type { ResultRecord } from '../../framework/backend/StreamFindApiClient';
import { localArtifactContract } from '../../framework/artifacts/artifactContracts';
import type { JsonValue } from '../../framework/backend/protocol';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationPointClick } from '../../framework/visualization/VisualizationRegistry';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import './QuantifySamplesViewer.css';

type QuantRow = Record<string, string | null>;
type TableRows = Record<string, QuantRow[]>;
type PlotTheme = { text: string; grid: string; background: string };
type Tab = 'calibration' | 'results';
type Metric = 'final_concentration' | 'response_ratio' | 'response';

const CONTRACTS = [
  'quantificationCalibrationModelsTable',
  'quantificationTable',
] as const;
const MODEL_COLUMNS = [
  'target_id',
  'model',
  'weighting',
  'intercept_mode',
  'coefficient_0',
  'coefficient_1',
  'coefficient_2',
  'r_squared',
  'adjusted_r_squared',
  'rmse',
  'n_points',
  'range_min',
  'range_max',
  'fit_status',
];
const CALIBRATION_COLUMNS = [
  'target_id',
  'analysis',
  'eic',
  'qualifier_eic',
  'peak_id',
  'calibration_level',
  'nominal_concentration',
  'response',
  'internal_standard_response',
  'response_ratio',
  'included',
  'back_calculated_concentration',
  'residual',
  'relative_residual',
  'exclusion_reason',
];
const QUANTIFICATION_QUERY_COLUMNS = [
  'analysis',
  'target_id',
  'role',
  'sample_type',
  'calibration_level',
  'nominal_concentration',
  'dilution_factor',
  'exclude',
  'internal_standard_id',
  'normalization',
  'chromatogram_index',
  'peak_id',
  'peak_rt',
  'eic_size',
  'eic_rt',
  'eic_intensity',
  'qualifier_eic_size',
  'qualifier_eic_rt',
  'qualifier_eic_intensity',
  'rt_error',
  'peak_area',
  'peak_height',
  'response_metric',
  'response',
  'internal_standard_response',
  'response_ratio',
  'qualifier_response',
  'qualifier_ratio',
  'status',
  'quantifier_status',
  'internal_standard_status',
  'candidate_count',
  'qualifier_status',
  'included',
  'back_calculated_concentration',
  'residual',
  'relative_residual',
  'exclusion_reason',
  'calculated_concentration',
  'final_concentration',
  'concentration_unit',
  'qualifier_ratio_pass',
  'accuracy_percent',
  'flags',
];
const QUANTIFICATION_COLUMNS = [
  'analysis',
  'target_id',
  'role',
  'sample_type',
  'nominal_concentration',
  'normalization',
  'chromatogram_index',
  'candidate_count',
  'eic',
  'qualifier_eic',
  'response_ratio',
  'response',
  'internal_standard_response',
  'peak_rt',
  'peak_area',
  'peak_height',
  'calculated_concentration',
  'final_concentration',
  'concentration_unit',
  'accuracy_percent',
  'accuracy_check',
  'included',
  'back_calculated_concentration',
  'relative_residual',
  'exclusion_reason',
  'qualifier_ratio',
  'qualifier_ratio_pass',
  'status',
  'quantifier_status',
  'internal_standard_status',
  'qualifier_status',
  'flags',
];

function theme(): PlotTheme {
  const get = (name: string, fallback: string) =>
    typeof document === 'undefined' ? fallback : getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  return { text: get('--sf-text', '#30343b'), grid: get('--sf-border', '#d5d9df'), background: get('--sf-surface-raised', '#ffffff') };
}

function text(row: QuantRow | undefined, column: string): string {
  return row?.[column] ?? '';
}

function number(row: QuantRow | undefined, column: string): number | null {
  const raw = row?.[column];
  if (raw === null || raw === undefined || raw === '') return null;
  const value = Number(raw);
  return Number.isFinite(value) ? value : null;
}

function numericArray(row: QuantRow, column: string): number[] {
  const raw = row[column];
  if (!raw) return [];
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed.map(Number).filter(Number.isFinite);
  } catch {
    return [];
  }
}

function quantificationRowKey(row: QuantRow): string {
  return [
    text(row, 'analysis'),
    text(row, 'target_id'),
    text(row, 'role'),
    text(row, 'sample_type'),
    text(row, 'chromatogram_index'),
    text(row, 'peak_id'),
  ].join('\u001f');
}

function plotRowKey(customdata: unknown): string | null {
  if (Array.isArray(customdata) && typeof customdata[0] === 'string') return customdata[0];
  return typeof customdata === 'string' ? customdata : null;
}

function EicSparkline({ row, prefix, label }: { row: QuantRow; prefix: string; label: string }) {
  const rt = numericArray(row, `${prefix}_rt`);
  const intensity = numericArray(row, `${prefix}_intensity`);
  const pointCount = Math.min(rt.length, intensity.length);
  if (pointCount < 2) return <span className="sf-quantification-eic-empty">—</span>;
  const values = intensity.slice(0, pointCount);
  const maximum = Math.max(...values, 0);
  const minimum = Math.min(...values, 0);
  const span = Math.max(maximum - minimum, 1);
  const points = values.map((value, index) => {
    const x = (index / Math.max(pointCount - 1, 1)) * 100;
    const y = 22 - ((value - minimum) / span) * 20;
    return `${x.toFixed(2)},${y.toFixed(2)}`;
  }).join(' ');
  return (
    <svg className="sf-quantification-eic" viewBox="0 0 100 24" role="img" aria-label={`${label} peak shape`}>
      <polyline points={`0,23 ${points} 100,23`} className="sf-quantification-eic-area" />
      <polyline points={points} className="sf-quantification-eic-line" />
    </svg>
  );
}

function format(value: string | null | undefined, digits = 4): string {
  if (value === null || value === undefined || value === '') return '—';
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return value;
  return parsed.toLocaleString('en-US', { maximumFractionDigits: digits, useGrouping: false });
}

function statusClass(value: string): string {
  const normalized = value.toLowerCase();
  if (normalized === 'ok' || normalized === 'matched' || normalized === 'pass' || normalized === 'within' || normalized === 'within limit') return 'is-good';
  if (normalized.includes('above') || normalized.includes('below') || normalized.includes('out_of_range')) return 'is-warning';
  if (normalized.includes('not_found') || normalized.includes('failed') || normalized.includes('invalid')) return 'is-bad';
  return 'is-neutral';
}

function accuracyStatus(row: QuantRow, accuracyLimit: number): string {
  const accuracy = number(row, 'accuracy_percent');
  if (accuracy === null) return '';
  if (accuracy < 100 - accuracyLimit) return 'below limit';
  if (accuracy > 100 + accuracyLimit) return 'above limit';
  return 'within limit';
}

function displayStatus(row: QuantRow, accuracyLimit: number): string {
  return accuracyStatus(row, accuracyLimit) || text(row, 'status') || 'unknown';
}

function nominalConcentration(row: QuantRow): number | null {
  return number(row, 'nominal_concentration');
}

function isDisplayedResultRow(row: QuantRow): boolean {
  return ['qc', 'unknown', 'blank'].includes(text(row, 'sample_type').toLowerCase());
}

function color(index: number): string {
  return `hsl(${(index * 137.508) % 360} 62% 48%)`;
}

function plotLayout(plotTheme: PlotTheme, xTitle: string, yTitle: string): Record<string, JsonValue> {
  return {
    autosize: true,
    margin: { l: 68, r: 20, t: 20, b: 58 },
    paper_bgcolor: plotTheme.background,
    plot_bgcolor: plotTheme.background,
    font: { color: plotTheme.text },
    hovermode: 'closest',
    hoverlabel: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
    legend: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
    xaxis: { title: { text: xTitle }, gridcolor: plotTheme.grid, zerolinecolor: plotTheme.grid, linecolor: plotTheme.text },
    yaxis: { title: { text: yTitle }, gridcolor: plotTheme.grid, zerolinecolor: plotTheme.grid, linecolor: plotTheme.text },
  };
}

function modelValue(model: QuantRow, x: number): number {
  const c0 = number(model, 'coefficient_0') ?? 0;
  const c1 = number(model, 'coefficient_1') ?? 0;
  const c2 = number(model, 'coefficient_2') ?? 0;
  return c0 + c1 * x + c2 * x * x;
}

function calibrationSpec(models: QuantRow[], points: QuantRow[], target: string, resultId: string, plotTheme: PlotTheme): VisualizationSpec | null {
  const selected = target === '__all__' ? models : models.filter((row) => text(row, 'target_id') === target);
  const traces: PlotlyTrace[] = [];
  selected.forEach((model, modelIndex) => {
    const targetId = text(model, 'target_id');
    const targetPoints = points.filter((row) => text(row, 'target_id') === targetId);
    const included = targetPoints.filter((row) => text(row, 'included') === 'true' && number(row, 'nominal_concentration') !== null);
    const excluded = targetPoints.filter((row) => text(row, 'included') !== 'true' && number(row, 'nominal_concentration') !== null);
    const yColumn = targetPoints.some((row) => number(row, 'response_ratio') !== null) ? 'response_ratio' : 'response';
    const allX = targetPoints.map((row) => number(row, 'nominal_concentration')).filter((value): value is number => value !== null);
    const min = number(model, 'range_min') ?? Math.min(...allX, 0);
    const max = number(model, 'range_max') ?? Math.max(...allX, 1);
    const curveX = Array.from({ length: 80 }, (_, index) => min + ((max - min) * index) / 79);
    traces.push({
      type: 'scattergl', mode: 'markers', name: `${targetId} raw`, legendgroup: targetId,
      x: included.map((row) => number(row, 'nominal_concentration')),
      y: included.map((row) => number(row, yColumn)), marker: { color: color(modelIndex), size: 8 },
      customdata: included.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
      hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>${yColumn}: %{y}<extra>included</extra>`,
    });
    if (excluded.length) traces.push({
      type: 'scattergl', mode: 'markers', name: `${targetId} excluded`, legendgroup: targetId,
      x: excluded.map((row) => number(row, 'nominal_concentration')),
      y: excluded.map((row) => number(row, yColumn)), marker: { color: color(modelIndex), size: 9, symbol: 'x' },
      customdata: excluded.map((row) => [quantificationRowKey(row), text(row, 'exclusion_reason') || 'excluded']),
      hovertemplate: `${targetId}<br>Nominal: %{x}<br>${yColumn}: %{y}<br>Reason: %{customdata[1]}<extra>excluded</extra>`,
    });
    traces.push({
      type: 'scattergl', mode: 'lines', name: `${targetId} fit`, legendgroup: targetId,
      x: curveX, y: curveX.map((x) => modelValue(model, x)), line: { color: color(modelIndex), width: 2.5 },
      hovertemplate: `${targetId} fit<br>Concentration: %{x}<br>Response: %{y}<extra></extra>`,
    });
  });
  if (!traces.length) return null;
  return {
    schema: 'streamfind.visualization/v1', visualization_id: 'mass_spec.quantification_calibration_viewer',
    semantic_type: 'mass_spec.quantification_calibration', title: 'Quantification calibration curves', data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: { data: traces, layout: plotLayout(plotTheme, 'Nominal concentration', 'Response / internal-standard ratio'), config: { responsive: true, displaylogo: false } },
    provenance: { source_result_ids: [resultId], producer_operation_id: 'frontend.mass_spec.quantification_viewer', producer_node_id: 'frontend.mass_spec.quantification_viewer' },
    fallback: { description: 'Quantification calibration visualization unavailable.' },
  };
}

function calibrationResidualSpec(models: QuantRow[], points: QuantRow[], target: string, resultId: string, plotTheme: PlotTheme): VisualizationSpec | null {
  const selected = target === '__all__' ? models : models.filter((row) => text(row, 'target_id') === target);
  const traces: PlotlyTrace[] = [];
  selected.forEach((model, modelIndex) => {
    const targetId = text(model, 'target_id');
    const targetPoints = points.filter((row) => text(row, 'target_id') === targetId && number(row, 'nominal_concentration') !== null && number(row, 'residual') !== null);
    const included = targetPoints.filter((row) => text(row, 'included') === 'true');
    const excluded = targetPoints.filter((row) => text(row, 'included') !== 'true');
    if (included.length) traces.push({
      type: 'scattergl', mode: 'markers', name: targetId, legendgroup: targetId,
      x: included.map((row) => number(row, 'nominal_concentration')), y: included.map((row) => number(row, 'residual')),
      marker: { color: color(modelIndex), size: 8 }, customdata: included.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
      hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>Residual: %{y}<extra>included</extra>`,
    });
    if (excluded.length) traces.push({
      type: 'scattergl', mode: 'markers', name: `${targetId} excluded`, legendgroup: targetId, showlegend: !included.length,
      x: excluded.map((row) => number(row, 'nominal_concentration')), y: excluded.map((row) => number(row, 'residual')),
      marker: { color: color(modelIndex), size: 9, symbol: 'x' }, customdata: excluded.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
      hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>Residual: %{y}<extra>excluded</extra>`,
    });
  });
  if (!traces.length) return null;
  const residuals = points.map((row) => number(row, 'residual')).filter((value): value is number => value !== null);
  const maximum = Math.max(...residuals.map((value) => Math.abs(value)), 1e-12);
  const maximumConcentration = Math.max(...points.map((row) => number(row, 'nominal_concentration') ?? 0), 1);
  traces.push({
    type: 'scattergl', mode: 'lines', name: 'zero residual', showlegend: false, hoverinfo: 'skip',
    x: [0, maximumConcentration], y: [0, 0], line: { color: plotTheme.grid, dash: 'dash', width: 1.2 },
  });
  return {
    schema: 'streamfind.visualization/v1', visualization_id: 'mass_spec.quantification_calibration_residuals_viewer',
    semantic_type: 'mass_spec.quantification_calibration_residuals', title: 'Calibration residuals', data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        ...plotLayout(plotTheme, 'Nominal concentration', 'Residual'),
        yaxis: { title: { text: 'Residual' }, range: [-maximum * 1.15, maximum * 1.15], gridcolor: plotTheme.grid, zerolinecolor: plotTheme.grid, linecolor: plotTheme.text },
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: { source_result_ids: [resultId], producer_operation_id: 'frontend.mass_spec.quantification_viewer', producer_node_id: 'frontend.mass_spec.quantification_viewer' },
    fallback: { description: 'Quantification calibration residual visualization unavailable.' },
  };
}

function resultsSpec(rows: QuantRow[], models: QuantRow[], target: string, metric: Metric, accuracyLimit: number, resultId: string, plotTheme: PlotTheme): VisualizationSpec | null {
  const selected = target === '__all__' ? rows : rows.filter((row) => text(row, 'target_id') === target);
  const targets = [...new Set(selected.map((row) => text(row, 'target_id')).filter(Boolean))];
  const traces: PlotlyTrace[] = [];
  targets.forEach((targetId, targetIndex) => {
    const targetRows = selected.filter((row) => text(row, 'target_id') === targetId && number(row, 'peak_rt') !== null && number(row, metric) !== null);
    const byStatus = new Map<string, QuantRow[]>();
    targetRows.forEach((row) => { const key = displayStatus(row, accuracyLimit); byStatus.set(key, [...(byStatus.get(key) ?? []), row]); });
    let legendShown = false;
    [...byStatus.entries()].forEach(([status, statusRows]) => {
      const groups = [
        { rows: statusRows.filter((row) => text(row, 'sample_type') === 'qc' && nominalConcentration(row) !== null), withAccuracyBar: true, symbol: 'diamond' },
        { rows: statusRows.filter((row) => text(row, 'sample_type') === 'qc' && nominalConcentration(row) === null), withAccuracyBar: false, symbol: 'diamond' },
        { rows: statusRows.filter((row) => text(row, 'sample_type') === 'blank'), withAccuracyBar: false, symbol: 'circle-open' },
        { rows: statusRows.filter((row) => !['qc', 'blank'].includes(text(row, 'sample_type')) && nominalConcentration(row) !== null), withAccuracyBar: true, symbol: 'circle' },
        { rows: statusRows.filter((row) => !['qc', 'blank'].includes(text(row, 'sample_type')) && nominalConcentration(row) === null), withAccuracyBar: false, symbol: 'circle' },
      ];
      groups.forEach(({ rows, withAccuracyBar, symbol }) => {
        if (!rows.length) return;
        const trace: PlotlyTrace = {
          type: 'scattergl', mode: 'markers', name: legendShown ? `${targetId} · ${status}` : targetId, legendgroup: targetId, showlegend: !legendShown,
          x: rows.map((row) => number(row, 'peak_rt')), y: rows.map((row) => number(row, metric)),
          marker: { color: color(targetIndex), symbol, size: 9 },
          customdata: rows.map((row) => [quantificationRowKey(row), text(row, 'analysis'), text(row, 'sample_type'), text(row, 'flags')]),
          hovertemplate: `${targetId}<br>RT: %{x:.3f} s<br>${metric}: %{y}<br>Analysis: %{customdata[1]}<br>Type: %{customdata[2]}<br>Flags: %{customdata[3]}<extra>${status}</extra>`,
        };
        legendShown = true;
        if (metric === 'final_concentration' && withAccuracyBar) {
          const lowerErrors = rows.map((row) => {
            const measured = number(row, metric) ?? 0;
            const nominal = nominalConcentration(row) ?? 0;
            return Math.max(0, measured - nominal * (1 - accuracyLimit / 100));
          });
          const upperErrors = rows.map((row) => {
            const measured = number(row, metric) ?? 0;
            const nominal = nominalConcentration(row) ?? 0;
            return Math.max(0, nominal * (1 + accuracyLimit / 100) - measured);
          });
          trace.error_y = { type: 'data', symmetric: false, array: upperErrors, arrayminus: lowerErrors, visible: true, color: color(targetIndex), thickness: 1.2, width: 4 };
        }
        traces.push(trace);
      });
    });
    const model = models.find((row) => text(row, 'target_id') === targetId);
    if (metric === 'final_concentration' && model && number(model, 'range_min') !== null && number(model, 'range_max') !== null) {
      const retentionTimes = targetRows.map((row) => number(row, 'peak_rt')).filter((value): value is number => value !== null);
      if (retentionTimes.length) {
        const minimumRt = Math.min(...retentionTimes);
        const maximumRt = Math.max(...retentionTimes);
        const padding = Math.max((maximumRt - minimumRt) * 0.03, 0.25);
        const x = [minimumRt - padding, maximumRt + padding];
        for (const [label, value, dash] of [
          ['lower calibration limit', number(model, 'range_min'), 'dash'],
          ['upper calibration limit', number(model, 'range_max'), 'dashdot'],
        ] as const) {
          traces.push({
            type: 'scattergl', mode: 'lines', name: `${targetId} ${label}`, legendgroup: targetId, showlegend: false,
            x, y: [value, value], line: { color: color(targetIndex), dash, width: 1.8 },
            hovertemplate: `${targetId}<br>${label}: %{y}<extra></extra>`,
          });
        }
      }
    }
  });
  if (!traces.length) return null;
  const yTitle = metric === 'final_concentration' ? 'Concentration' : metric === 'response_ratio' ? 'Response ratio' : 'Response';
  return {
    schema: 'streamfind.visualization/v1', visualization_id: 'mass_spec.quantification_results_viewer', semantic_type: 'mass_spec.quantification_results',
    title: 'Quantification results', data_mode: 'inline', renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: { data: traces, layout: plotLayout(plotTheme, 'Peak retention time (s)', yTitle), config: { responsive: true, displaylogo: false } },
    provenance: { source_result_ids: [resultId], producer_operation_id: 'frontend.mass_spec.quantification_viewer', producer_node_id: 'frontend.mass_spec.quantification_viewer' },
    fallback: { description: 'Quantification results visualization unavailable.' },
  };
}

function DataTable({ title, rows, columns, selectedRowKey, onRowSelect, limit = 1000, accuracyLimit = 20, fillHeight = false }: {
  title: string;
  rows: QuantRow[];
  columns: string[];
  selectedRowKey?: string | null;
  onRowSelect?: (key: string) => void;
  limit?: number;
  accuracyLimit?: number;
  fillHeight?: boolean;
}) {
  const selectedRowRef = useRef<HTMLTableRowElement | null>(null);
  useEffect(() => {
    selectedRowRef.current?.scrollIntoView({ block: 'nearest' });
  }, [selectedRowKey]);
  const visibleRows = rows.slice(0, limit);
  const selectedOutsidePage = selectedRowKey && !visibleRows.some((row) => quantificationRowKey(row) === selectedRowKey)
    ? rows.find((row) => quantificationRowKey(row) === selectedRowKey)
    : undefined;
  if (selectedOutsidePage) visibleRows.push(selectedOutsidePage);
  return <section className={`sf-quantification-table-card${fillHeight ? ' sf-quantification-table-card--fill' : ''}`}>
    <h3>{title} <small>{rows.length} rows</small></h3>
    <div className="sf-quantification-table-scroll">
      <table>
        <thead><tr>{columns.map((column) => <th key={column}>
          {column === 'eic' ? 'Quantifier EIC' : column === 'qualifier_eic' ? 'Qualifier EIC' : column.replaceAll('_', ' ')}
        </th>)}</tr></thead>
        <tbody>{visibleRows.map((row, index) => {
          const key = quantificationRowKey(row);
          const selected = key === selectedRowKey;
          return <tr
            key={`${key}-${index}`}
            ref={(element) => { if (selected) selectedRowRef.current = element; }}
            className={selected ? 'is-selected' : undefined}
            aria-current={selected ? 'true' : undefined}
            onClick={() => onRowSelect?.(key)}
          >{columns.map((column) => {
            if (column === 'eic' || column === 'qualifier_eic') {
              const prefix = column === 'eic' ? 'eic' : 'qualifier_eic';
              const label = column === 'eic' ? 'Quantifier EIC' : 'Qualifier EIC';
              return <td key={column}><EicSparkline row={row} prefix={prefix} label={label} /></td>;
            }
            const value = column === 'accuracy_check' ? accuracyStatus(row, accuracyLimit) : text(row, column);
            const isStatus = ['status', 'fit_status', 'quantifier_status', 'internal_standard_status', 'qualifier_status', 'accuracy_check'].includes(column);
            return <td key={column} className={isStatus ? statusClass(value) : undefined}>{isStatus ? <span className="sf-quantification-status">{value || '—'}</span> : format(value, column.includes('rt') ? 3 : 4)}</td>;
          })}</tr>;
        })}</tbody>
      </table>
    </div>
    {rows.length > limit && <p className="sf-quantification-note">Showing the first {limit} rows.</p>}
  </section>;
}

export function QuantifySamplesViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [tables, setTables] = useState<TableRows>({});
  const [loadState, setLoadState] = useState<{ resultId: string; error?: string } | null>(null);
  const [tab, setTab] = useState<Tab>('calibration');
  const [target, setTarget] = useState('__all__');
  const [metric, setMetric] = useState<Metric>('final_concentration');
  const [accuracyLimit, setAccuracyLimit] = useState(20);
  const [selectedRowKey, setSelectedRowKey] = useState<string | null>(null);
  const resultId = artifact?.result_id ?? '';
  const loading = !loadState || loadState.resultId !== resultId;
  const error = loadState?.resultId === resultId ? loadState.error : undefined;
  useEffect(() => {
    if (!artifact || !client) return undefined;
    let cancelled = false;
    void client.currentResults(context.sessionId).then(async (inventory) => {
      const siblings = inventory.filter((candidate) => candidate.producer_instance === artifact.producer_instance && CONTRACTS.includes(localArtifactContract(candidate.contract_id) as (typeof CONTRACTS)[number]));
      if (!siblings.some((candidate) => candidate.result_id === artifact.result_id)) siblings.push(artifact);
      const loaded = await Promise.all(siblings.map(async (candidate) => {
        const contract = localArtifactContract(candidate.contract_id);
        const columns = contract === 'quantificationCalibrationModelsTable' ? MODEL_COLUMNS : QUANTIFICATION_QUERY_COLUMNS;
        const response = await client.resultQuery(context.sessionId, { result_id: candidate.result_id, mode: 'page', columns, limit: 10000 });
        return [contract, response.rows] as const;
      }));
      if (cancelled) return;
      setTables(Object.fromEntries(loaded));
      setLoadState({ resultId });
    }).catch((reason: unknown) => { if (!cancelled) setLoadState({ resultId, error: reason instanceof Error ? reason.message : 'Unable to load quantification results.' }); });
    return () => { cancelled = true; };
  }, [artifact, client, context.sessionId, resultId]);

  const models = tables.quantificationCalibrationModelsTable ?? [];
  const quantification = tables.quantificationTable ?? [];
  const points = quantification.filter((row) => text(row, 'role') === 'analyte' && text(row, 'sample_type') === 'calibrator');
  const resultRows = quantification.filter(isDisplayedResultRow);
  const results = resultRows.filter((row) => text(row, 'role') === 'analyte');
  const targets = [...new Set([...models, ...results].map((row) => text(row, 'target_id')).filter(Boolean))];
  const plotTheme = theme();
  const calibrationPlot = useMemo(() => calibrationSpec(models, points, target, resultId, plotTheme), [models, points, target, resultId, plotTheme]);
  const residualPlot = useMemo(() => calibrationResidualSpec(models, points, target, resultId, plotTheme), [models, points, target, resultId, plotTheme]);
  const resultPlot = useMemo(() => resultsSpec(results, models, target, metric, accuracyLimit, resultId, plotTheme), [results, models, target, metric, accuracyLimit, resultId, plotTheme]);
  const handlePlotPointClick = (point: VisualizationPointClick) => {
    const key = plotRowKey(point.customdata);
    if (key) setSelectedRowKey(key);
  };

  if (!artifact || !client) return <div role="alert">Quantification result data is unavailable.</div>;
  if (loading) return <LoadingLogo label="Loading quantification results…" />;
  if (error) return <div className="sf-quantification-viewer-state" role="alert">{error}</div>;
  return <div className="sf-quantification-viewer">
    <div className="sf-quantification-viewer-toolbar" aria-label="Quantification viewer controls">
      <div className="sf-quantification-tabs" role="tablist" aria-label="Quantification views">
        <button className={tab === 'calibration' ? 'is-active' : ''} role="tab" aria-selected={tab === 'calibration'} onClick={() => setTab('calibration')}>Calibration curves</button>
        <button className={tab === 'results' ? 'is-active' : ''} role="tab" aria-selected={tab === 'results'} onClick={() => setTab('results')}>Responses & results</button>
      </div>
      <label>Compound <select value={target} onChange={(event) => setTarget(event.target.value)}><option value="__all__">All compounds</option>{targets.map((value) => <option key={value} value={value}>{value}</option>)}</select></label>
      {tab === 'results' && <><label>Plot <select value={metric} onChange={(event) => setMetric(event.target.value as Metric)}><option value="final_concentration">Final concentration</option><option value="response_ratio">Response ratio</option><option value="response">Response</option></select></label><label>Accuracy limit ± <input type="number" min="0" max="100" step="1" value={accuracyLimit} onChange={(event) => setAccuracyLimit(Number(event.target.value) || 0)} /> %</label></>}
      <span className="sf-quantification-summary">{models.length} models · {quantification.length} quantification rows</span>
    </div>
    {tab === 'calibration' ? <>
      <div className="sf-quantification-calibration-plots">
        <section><h3>Calibration model</h3><div className="sf-quantification-plot">{calibrationPlot ? <VisualizationRenderer spec={calibrationPlot} onPointClick={handlePlotPointClick} /> : <div className="sf-quantification-viewer-state">No calibration points are available.</div>}</div></section>
        <section><h3>Residuals</h3><div className="sf-quantification-plot">{residualPlot ? <VisualizationRenderer spec={residualPlot} onPointClick={handlePlotPointClick} /> : <div className="sf-quantification-viewer-state">No residuals are available.</div>}</div></section>
      </div>
      <DataTable title="Calibration models" rows={models.filter((row) => target === '__all__' || text(row, 'target_id') === target)} columns={MODEL_COLUMNS} />
      <DataTable title="Raw calibration points" rows={points.filter((row) => target === '__all__' || text(row, 'target_id') === target)} columns={CALIBRATION_COLUMNS} selectedRowKey={selectedRowKey} onRowSelect={setSelectedRowKey} fillHeight />
    </> : <>
      <div className="sf-quantification-plot">{resultPlot ? <VisualizationRenderer spec={resultPlot} onPointClick={handlePlotPointClick} /> : <div className="sf-quantification-viewer-state">No quantification results are available.</div>}</div>
      <DataTable title="QC, unknown & blank results" rows={resultRows.filter((row) => target === '__all__' || text(row, 'target_id') === target)} columns={QUANTIFICATION_COLUMNS} selectedRowKey={selectedRowKey} onRowSelect={setSelectedRowKey} accuracyLimit={accuracyLimit} fillHeight />
    </>}
  </div>;
}
