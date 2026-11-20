import { useEffect, useId, useMemo, useRef, useState } from 'react';
import { localArtifactContract } from '../../framework/artifacts/artifactContracts';
import type { JsonValue } from '../../framework/backend/protocol';
import { LoadingLogo } from '../../framework/ui/LoadingLogo';
import { VisualizationRenderer } from '../../framework/visualization/VisualizationRenderer';
import type { VisualizationPointClick } from '../../framework/visualization/VisualizationRegistry';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';
import './QuantifySamplesViewer.css';
import { MassSpecFilterToolbar, type MassSpecFilterValues } from './MassSpecFilterToolbar';
import { filterValueMatches } from './massSpecFilterUtils';

type QuantRow = Record<string, string | null>;
type TableRows = Record<string, QuantRow[]>;
type PlotTheme = { text: string; grid: string; background: string };
type Tab = 'calibration' | 'results';
type Metric = 'final_concentration' | 'response_ratio' | 'response';

const CONTRACTS = ['quantificationCalibrationModelsTable', 'quantificationTable'] as const;
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
  'calibration_limit_min',
  'calibration_limit_max',
  'distinct_levels',
  'min_replicates_per_level',
  'max_replicates_per_level',
  'replicate_mode',
  'replicate_status',
  'calibration_acceptance_status',
  'domain_status',
  'monotonicity_status',
  'fit_status',
];
const CALIBRATION_COLUMNS = [
  'target_id',
  'analysis',
  'eic',
  'qualifier_eic',
  'internal_standard_eic',
  'peak_id',
  'peak_rt_start',
  'peak_rt_end',
  'qualifier_peak_rt_start',
  'qualifier_peak_rt_end',
  'internal_standard_peak_rt_start',
  'internal_standard_peak_rt_end',
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
  'calibration_level_status',
  'calibration_replicate_count',
  'candidate_peak_ids',
  'qualifier_candidate_peak_ids',
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
  'internal_standard_eic_size',
  'internal_standard_eic_rt',
  'internal_standard_eic_intensity',
  'peak_rt_start',
  'peak_rt_end',
  'qualifier_peak_rt_start',
  'qualifier_peak_rt_end',
  'internal_standard_peak_rt_start',
  'internal_standard_peak_rt_end',
  'quantifier_chromatogram_id',
  'qualifier_chromatogram_id',
  'qualifier_chromatogram_index',
  'qualifier_peak_id',
  'qualifier_peak_rt',
  'qualifier_peak_area',
  'qualifier_peak_height',
  'qualifier_rt_error',
  'qualifier_candidate_count',
  'candidate_peak_ids',
  'qualifier_candidate_peak_ids',
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
  'calibration_range_status',
  'calibration_level_status',
  'calibration_replicate_count',
  'qc_status',
  'blank_status',
  'carryover_status',
  'dilution_status',
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
  'internal_standard_eic',
  'peak_rt_start',
  'peak_rt_end',
  'qualifier_peak_rt_start',
  'qualifier_peak_rt_end',
  'internal_standard_peak_rt_start',
  'internal_standard_peak_rt_end',
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
  'calibration_range_status',
  'qc_status',
  'blank_status',
  'carryover_status',
  'dilution_status',
  'flags',
];

function theme(): PlotTheme {
  const get = (name: string, fallback: string) =>
    typeof document === 'undefined'
      ? fallback
      : getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
  return {
    text: get('--sf-text', '#30343b'),
    grid: get('--sf-border', '#d5d9df'),
    background: get('--sf-surface-raised', '#ffffff'),
  };
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

function matchesFilters(row: QuantRow, filters: MassSpecFilterValues): boolean {
  return Object.entries(filters).every(
    ([column, values]) => !values.length || filterValueMatches(column, text(row, column), values),
  );
}

function plotRowKey(customdata: unknown): string | null {
  if (Array.isArray(customdata) && typeof customdata[0] === 'string') return customdata[0];
  return typeof customdata === 'string' ? customdata : null;
}

function eicBoundaryColumns(prefix: string): [string, string] {
  const channel = prefix === 'eic' ? '' : prefix.replace(/_eic$/, '');
  return [
    channel ? `${channel}_peak_rt_start` : 'peak_rt_start',
    channel ? `${channel}_peak_rt_end` : 'peak_rt_end',
  ];
}

function integrationWindow(row: QuantRow, prefix: string): [number, number] | null {
  const [startColumn, endColumn] = eicBoundaryColumns(prefix);
  const start = number(row, startColumn);
  const end = number(row, endColumn);
  return start !== null && end !== null && end > start ? [start, end] : null;
}

function EicSparkline({ row, prefix, label }: { row: QuantRow; prefix: string; label: string }) {
  const clipId = `sf-eic-window-${useId().replace(/:/g, '')}`;
  const rt = numericArray(row, `${prefix}_rt`);
  const intensity = numericArray(row, `${prefix}_intensity`);
  const pointCount = Math.min(rt.length, intensity.length);
  if (pointCount < 2) return <span className="sf-quantification-eic-empty">—</span>;
  const values = intensity.slice(0, pointCount);
  const xValues = rt.slice(0, pointCount);
  const maximum = Math.max(...values, 0);
  const minimum = Math.min(...values, 0);
  const span = Math.max(maximum - minimum, 1);
  const minimumX = Math.min(...xValues);
  const maximumX = Math.max(...xValues);
  const xSpan = Math.max(maximumX - minimumX, 1e-12);
  const points = values
    .map((value, index) => {
      const x = ((xValues[index] - minimumX) / xSpan) * 100;
      const y = 22 - ((value - minimum) / span) * 20;
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    })
    .join(' ');
  const window = integrationWindow(row, prefix);
  const windowStart = window ? Math.max(0, ((window[0] - minimumX) / xSpan) * 100) : 0;
  const windowEnd = window ? Math.min(100, ((window[1] - minimumX) / xSpan) * 100) : 0;
  return (
    <svg className="sf-quantification-eic" viewBox="0 0 100 24" role="img" aria-label={`${label} peak shape`}>
      {window && windowEnd > windowStart && (
        <clipPath id={clipId}>
          <rect x={windowStart} y="0" width={windowEnd - windowStart} height="24" />
        </clipPath>
      )}
      {window && windowEnd > windowStart && (
        <polyline
          points={`0,23 ${points} 100,23`}
          className="sf-quantification-eic-area"
          clipPath={`url(#${clipId})`}
        />
      )}
      <polyline points={points} className="sf-quantification-eic-line" />
      {window && windowEnd > windowStart && (
        <>
          <line x1={windowStart} x2={windowStart} y1="2" y2="23" className="sf-quantification-eic-boundary" />
          <line x1={windowEnd} x2={windowEnd} y1="2" y2="23" className="sf-quantification-eic-boundary" />
        </>
      )}
    </svg>
  );
}

function EicDetailPlot({ row, prefix, label }: { row: QuantRow; prefix: string; label: string }) {
  const clipId = `sf-eic-detail-window-${useId().replace(/:/g, '')}`;
  const rt = numericArray(row, `${prefix}_rt`);
  const intensity = numericArray(row, `${prefix}_intensity`);
  const pointCount = Math.min(rt.length, intensity.length);
  if (pointCount < 2) return <div className="sf-quantification-detail-eic-empty">No EIC trace available</div>;
  const xValues = rt.slice(0, pointCount);
  const yValues = intensity.slice(0, pointCount);
  const width = 640;
  const height = 190;
  const padding = { left: 46, right: 14, top: 18, bottom: 32 };
  const plotWidth = width - padding.left - padding.right;
  const plotHeight = height - padding.top - padding.bottom;
  const minimumX = Math.min(...xValues);
  const maximumX = Math.max(...xValues);
  const minimumY = Math.min(...yValues, 0);
  const maximumY = Math.max(...yValues, 0);
  const xSpan = Math.max(maximumX - minimumX, 1e-12);
  const ySpan = Math.max(maximumY - minimumY, 1e-12);
  const mapX = (value: number) => padding.left + ((value - minimumX) / xSpan) * plotWidth;
  const points = yValues
    .map((value, index) => {
      const x = mapX(xValues[index]);
      const y = padding.top + plotHeight - ((value - minimumY) / ySpan) * plotHeight;
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    })
    .join(' ');
  const baselineY = padding.top + plotHeight - ((0 - minimumY) / ySpan) * plotHeight;
  const window = integrationWindow(row, prefix);
  const windowStartX = window ? Math.max(padding.left, mapX(window[0])) : padding.left;
  const windowEndX = window ? Math.min(width - padding.right, mapX(window[1])) : padding.left;
  return (
    <svg
      className="sf-quantification-detail-eic"
      viewBox={`0 0 ${width} ${height}`}
      role="img"
      aria-label={`${label} detail`}
    >
      {window && windowEndX > windowStartX && (
        <clipPath id={clipId}>
          <rect x={windowStartX} y={padding.top} width={windowEndX - windowStartX} height={plotHeight} />
        </clipPath>
      )}
      <line x1={padding.left} x2={width - padding.right} y1={baselineY} y2={baselineY} className="is-axis" />
      {window && windowEndX > windowStartX && (
        <>
          <rect
            x={windowStartX}
            y={padding.top}
            width={windowEndX - windowStartX}
            height={plotHeight}
            className="is-integration-window"
          />
          <polyline
            points={`${padding.left},${baselineY} ${points} ${width - padding.right},${baselineY}`}
            className="is-area"
            clipPath={`url(#${clipId})`}
          />
          <line x1={windowStartX} x2={windowStartX} y1={padding.top} y2={padding.top + plotHeight} className="is-boundary" />
          <line x1={windowEndX} x2={windowEndX} y1={padding.top} y2={padding.top + plotHeight} className="is-boundary" />
          <text x={windowStartX + 3} y={padding.top + 12} className="is-boundary-label">start</text>
          <text x={windowEndX - 3} y={padding.top + 12} textAnchor="end" className="is-boundary-label">end</text>
        </>
      )}
      <polyline points={points} className="is-line" />
      <text x={padding.left} y={height - 9} className="is-axis-label">
        {format(String(minimumX), 3)}
      </text>
      <text x={width - padding.right} y={height - 9} textAnchor="end" className="is-axis-label">
        {format(String(maximumX), 3)}
      </text>
      <text x={padding.left - 8} y={padding.top + 4} textAnchor="end" className="is-axis-label">
        {format(String(maximumY), 3)}
      </text>
      <text x={padding.left - 8} y={baselineY + 4} textAnchor="end" className="is-axis-label">
        0
      </text>
      <text x={width - padding.right} y={height - 9} textAnchor="end" className="is-axis-label">
        {window ? `integrated ${format(String(window[0]), 3)}–${format(String(window[1]), 3)}` : 'integration boundaries unavailable'}
      </text>
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
  if (
    normalized === 'ok' ||
    normalized === 'matched' ||
    normalized === 'pass' ||
    normalized === 'accepted' ||
    normalized === 'in_range' ||
    normalized === 'within' ||
    normalized === 'within limit'
  )
    return 'is-good';
  if (normalized.includes('above') || normalized.includes('below') || normalized.includes('out_of_range'))
    return 'is-warning';
  if (normalized.includes('not_found') || normalized.includes('failed') || normalized.includes('invalid'))
    return 'is-bad';
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
  return text(row, 'role') === 'analyte' && ['qc', 'unknown', 'blank'].includes(text(row, 'sample_type').toLowerCase());
}

function color(index: number): string {
  return `hsl(${(index * 137.508) % 360} 62% 48%)`;
}

function plotLayout(plotTheme: PlotTheme, xTitle: string, yTitle: string): Record<string, JsonValue> {
  return {
    autosize: true,
    margin: { l: 88, r: 20, t: 20, b: 58 },
    paper_bgcolor: plotTheme.background,
    plot_bgcolor: plotTheme.background,
    font: { color: plotTheme.text },
    hovermode: 'closest',
    hoverlabel: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
    legend: { bgcolor: plotTheme.background, font: { color: plotTheme.text } },
    xaxis: {
      title: { text: xTitle },
      gridcolor: plotTheme.grid,
      zerolinecolor: plotTheme.grid,
      linecolor: plotTheme.text,
    },
    yaxis: {
      title: { text: yTitle },
      gridcolor: plotTheme.grid,
      zerolinecolor: plotTheme.grid,
      linecolor: plotTheme.text,
    },
  };
}

function modelValue(model: QuantRow, x: number): number {
  const c0 = number(model, 'coefficient_0') ?? 0;
  const c1 = number(model, 'coefficient_1') ?? 0;
  const c2 = number(model, 'coefficient_2') ?? 0;
  if (text(model, 'model') === 'wagner') {
    const logX = Math.log(x);
    return Math.exp(c0 + c1 * logX + c2 * logX * logX);
  }
  return c0 + c1 * x + c2 * x * x;
}

function downloadRows(filename: string, rows: QuantRow[], columns: string[], format: 'csv' | 'json') {
  if (typeof window === 'undefined') return;
  const payload = format === 'json'
    ? JSON.stringify(rows.map((row) => Object.fromEntries(columns.map((column) => [column, row[column] ?? null]))), null, 2)
    : [
        columns.join(','),
        ...rows.map((row) => columns.map((column) => {
          const value = row[column] ?? '';
          return /[",\n]/.test(value) ? `"${value.replaceAll('"', '""')}"` : value;
        }).join(',')),
      ].join('\n');
  const blob = new Blob([payload], { type: format === 'json' ? 'application/json' : 'text/csv' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  link.click();
  URL.revokeObjectURL(url);
}

function calibrationSpec(
  models: QuantRow[],
  points: QuantRow[],
  target: string,
  resultId: string,
  plotTheme: PlotTheme,
): VisualizationSpec | null {
  const selected = target === '__all__' ? models : models.filter((row) => text(row, 'target_id') === target);
  const traces: PlotlyTrace[] = [];
  selected.forEach((model, modelIndex) => {
    const targetId = text(model, 'target_id');
    const targetPoints = points.filter((row) => text(row, 'target_id') === targetId);
    const included = targetPoints.filter(
      (row) => text(row, 'included') === 'true' && number(row, 'nominal_concentration') !== null,
    );
    const excluded = targetPoints.filter(
      (row) => text(row, 'included') !== 'true' && number(row, 'nominal_concentration') !== null,
    );
    const yColumn = targetPoints.some((row) => number(row, 'response_ratio') !== null) ? 'response_ratio' : 'response';
    const allX = targetPoints
      .map((row) => number(row, 'nominal_concentration'))
      .filter((value): value is number => value !== null);
    const min = number(model, 'range_min') ?? Math.min(...allX, 0);
    const max = number(model, 'range_max') ?? Math.max(...allX, 1);
    const modelName = text(model, 'model') || 'linear';
    const curveX =
      modelName === 'wagner'
        ? Array.from({ length: 80 }, (_, index) =>
            Math.exp(Math.log(min) + ((Math.log(max) - Math.log(min)) * index) / 79),
          )
        : Array.from({ length: 80 }, (_, index) => min + ((max - min) * index) / 79);
    traces.push({
      type: 'scattergl',
      mode: 'markers',
      name: targetId,
      showlegend: included.length > 0,
      legendgroup: targetId,
      x: included.map((row) => number(row, 'nominal_concentration')),
      y: included.map((row) => number(row, yColumn)),
      marker: { color: color(modelIndex), size: 8 },
      customdata: included.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
      hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>${yColumn}: %{y}<extra>included</extra>`,
    });
    if (excluded.length)
      traces.push({
        type: 'scattergl',
        mode: 'markers',
        name: targetId,
        showlegend: included.length === 0,
        legendgroup: targetId,
        x: excluded.map((row) => number(row, 'nominal_concentration')),
        y: excluded.map((row) => number(row, yColumn)),
        marker: { color: color(modelIndex), size: 9, symbol: 'x' },
        customdata: excluded.map((row) => [quantificationRowKey(row), text(row, 'exclusion_reason') || 'excluded']),
        hovertemplate: `${targetId}<br>Nominal: %{x}<br>${yColumn}: %{y}<br>Reason: %{customdata[1]}<extra>excluded</extra>`,
      });
    if (text(model, 'fit_status') === 'ok')
      traces.push({
        type: 'scattergl',
        mode: 'lines',
        name: targetId,
        showlegend: false,
        legendgroup: targetId,
        x: curveX,
        y: curveX.map((x) => modelValue(model, x)),
        line: { color: color(modelIndex), width: 2.5 },
        hovertemplate: `${targetId} ${modelName} fit<br>Concentration: %{x}<br>Response: %{y}<extra></extra>`,
      });
  });
  if (!traces.length) return null;
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.quantification_calibration_viewer',
    semantic_type: 'mass_spec.quantification_calibration',
    title: 'Quantification calibration curves',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: plotLayout(plotTheme, 'Nominal concentration', 'Response /<br>internal-standard ratio'),
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_result_ids: [resultId],
      producer_operation_id: 'frontend.mass_spec.quantification_viewer',
      producer_node_id: 'frontend.mass_spec.quantification_viewer',
    },
    fallback: { description: 'Quantification calibration visualization unavailable.' },
  };
}

function calibrationResidualSpec(
  models: QuantRow[],
  points: QuantRow[],
  target: string,
  resultId: string,
  plotTheme: PlotTheme,
): VisualizationSpec | null {
  const selected = target === '__all__' ? models : models.filter((row) => text(row, 'target_id') === target);
  const traces: PlotlyTrace[] = [];
  selected.forEach((model, modelIndex) => {
    const targetId = text(model, 'target_id');
    const targetPoints = points.filter(
      (row) =>
        text(row, 'target_id') === targetId &&
        number(row, 'nominal_concentration') !== null &&
        number(row, 'residual') !== null,
    );
    const included = targetPoints.filter((row) => text(row, 'included') === 'true');
    const excluded = targetPoints.filter((row) => text(row, 'included') !== 'true');
    if (included.length)
      traces.push({
        type: 'scattergl',
        mode: 'markers',
        name: targetId,
        legendgroup: targetId,
        x: included.map((row) => number(row, 'nominal_concentration')),
        y: included.map((row) => number(row, 'residual')),
        marker: { color: color(modelIndex), size: 8 },
        customdata: included.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
        hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>Residual: %{y}<extra>included</extra>`,
      });
    if (excluded.length)
      traces.push({
        type: 'scattergl',
        mode: 'markers',
        name: `${targetId} excluded`,
        legendgroup: targetId,
        showlegend: !included.length,
        x: excluded.map((row) => number(row, 'nominal_concentration')),
        y: excluded.map((row) => number(row, 'residual')),
        marker: { color: color(modelIndex), size: 9, symbol: 'x' },
        customdata: excluded.map((row) => [quantificationRowKey(row), text(row, 'analysis')]),
        hovertemplate: `${targetId}<br>Analysis: %{customdata[1]}<br>Nominal: %{x}<br>Residual: %{y}<extra>excluded</extra>`,
      });
  });
  if (!traces.length) return null;
  const residuals = points.map((row) => number(row, 'residual')).filter((value): value is number => value !== null);
  const maximum = Math.max(...residuals.map((value) => Math.abs(value)), 1e-12);
  const maximumConcentration = Math.max(...points.map((row) => number(row, 'nominal_concentration') ?? 0), 1);
  traces.push({
    type: 'scattergl',
    mode: 'lines',
    name: 'zero residual',
    showlegend: false,
    hoverinfo: 'skip',
    x: [0, maximumConcentration],
    y: [0, 0],
    line: { color: plotTheme.grid, dash: 'dash', width: 1.2 },
  });
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.quantification_calibration_residuals_viewer',
    semantic_type: 'mass_spec.quantification_calibration_residuals',
    title: 'Calibration residuals',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        ...plotLayout(
          plotTheme,
          'Nominal concentration',
          models.some((model) => text(model, 'model') === 'wagner') ? 'Log-response residual' : 'Residual',
        ),
        yaxis: {
          title: {
            text: models.some((model) => text(model, 'model') === 'wagner') ? 'Log-response residual' : 'Residual',
          },
          range: [-maximum * 1.15, maximum * 1.15],
          gridcolor: plotTheme.grid,
          zerolinecolor: plotTheme.grid,
          linecolor: plotTheme.text,
        },
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_result_ids: [resultId],
      producer_operation_id: 'frontend.mass_spec.quantification_viewer',
      producer_node_id: 'frontend.mass_spec.quantification_viewer',
    },
    fallback: { description: 'Quantification calibration residual visualization unavailable.' },
  };
}

function resultsSpec(
  rows: QuantRow[],
  models: QuantRow[],
  target: string,
  metric: Metric,
  accuracyLimit: number,
  resultId: string,
  plotTheme: PlotTheme,
): VisualizationSpec | null {
  const selected = target === '__all__' ? rows : rows.filter((row) => text(row, 'target_id') === target);
  const targets = [...new Set(selected.map((row) => text(row, 'target_id')).filter(Boolean))];
  const traces: PlotlyTrace[] = [];
  targets.forEach((targetId, targetIndex) => {
    const targetRows = selected.filter(
      (row) => text(row, 'target_id') === targetId && number(row, 'peak_rt') !== null && number(row, metric) !== null,
    );
    const byStatus = new Map<string, QuantRow[]>();
    targetRows.forEach((row) => {
      const key = displayStatus(row, accuracyLimit);
      byStatus.set(key, [...(byStatus.get(key) ?? []), row]);
    });
    let legendShown = false;
    [...byStatus.entries()].forEach(([status, statusRows]) => {
      const groups = [
        {
          rows: statusRows.filter((row) => text(row, 'sample_type') === 'qc' && nominalConcentration(row) !== null),
          withAccuracyBar: true,
          symbol: 'diamond',
        },
        {
          rows: statusRows.filter((row) => text(row, 'sample_type') === 'qc' && nominalConcentration(row) === null),
          withAccuracyBar: false,
          symbol: 'diamond',
        },
        {
          rows: statusRows.filter((row) => text(row, 'sample_type') === 'blank'),
          withAccuracyBar: false,
          symbol: 'circle-open',
        },
        {
          rows: statusRows.filter(
            (row) => !['qc', 'blank'].includes(text(row, 'sample_type')) && nominalConcentration(row) !== null,
          ),
          withAccuracyBar: true,
          symbol: 'circle',
        },
        {
          rows: statusRows.filter(
            (row) => !['qc', 'blank'].includes(text(row, 'sample_type')) && nominalConcentration(row) === null,
          ),
          withAccuracyBar: false,
          symbol: 'circle',
        },
      ];
      groups.forEach(({ rows, withAccuracyBar, symbol }) => {
        if (!rows.length) return;
        const trace: PlotlyTrace = {
          type: 'scattergl',
          mode: 'markers',
          name: legendShown ? `${targetId} · ${status}` : targetId,
          legendgroup: targetId,
          showlegend: !legendShown,
          x: rows.map((row) => number(row, 'peak_rt')),
          y: rows.map((row) => number(row, metric)),
          marker: { color: color(targetIndex), symbol, size: 9 },
          customdata: rows.map((row) => [
            quantificationRowKey(row),
            text(row, 'analysis'),
            text(row, 'sample_type'),
            text(row, 'flags'),
          ]),
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
          trace.error_y = {
            type: 'data',
            symmetric: false,
            array: upperErrors,
            arrayminus: lowerErrors,
            visible: true,
            color: color(targetIndex),
            thickness: 1.2,
            width: 4,
          };
        }
        traces.push(trace);
      });
    });
    const model = models.find((row) => text(row, 'target_id') === targetId);
    if (
      metric === 'final_concentration' &&
      model &&
      number(model, 'range_min') !== null &&
      number(model, 'range_max') !== null
    ) {
      const retentionTimes = targetRows
        .map((row) => number(row, 'peak_rt'))
        .filter((value): value is number => value !== null);
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
            type: 'scattergl',
            mode: 'lines',
            name: `${targetId} ${label}`,
            legendgroup: targetId,
            showlegend: false,
            x,
            y: [value, value],
            line: { color: color(targetIndex), dash, width: 1.8 },
            hovertemplate: `${targetId}<br>${label}: %{y}<extra></extra>`,
          });
        }
      }
    }
  });
  if (!traces.length) return null;
  const yTitle =
    metric === 'final_concentration' ? 'Concentration' : metric === 'response_ratio' ? 'Response ratio' : 'Response';
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: 'mass_spec.quantification_results_viewer',
    semantic_type: 'mass_spec.quantification_results',
    title: 'Quantification results',
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: plotLayout(plotTheme, 'Peak retention time (s)', yTitle),
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_result_ids: [resultId],
      producer_operation_id: 'frontend.mass_spec.quantification_viewer',
      producer_node_id: 'frontend.mass_spec.quantification_viewer',
    },
    fallback: { description: 'Quantification results visualization unavailable.' },
  };
}

function DataTable({
  title,
  rows,
  columns,
  selectedRowKey,
  onRowSelect,
  onRowOpen,
  limit = 1000,
  accuracyLimit = 20,
  fillHeight = false,
  showHeader = true,
}: {
  title: string;
  rows: QuantRow[];
  columns: string[];
  selectedRowKey?: string | null;
  onRowSelect?: (key: string) => void;
  onRowOpen?: (row: QuantRow) => void;
  limit?: number;
  accuracyLimit?: number;
  fillHeight?: boolean;
  showHeader?: boolean;
}) {
  const selectedRowRef = useRef<HTMLTableRowElement | null>(null);
  useEffect(() => {
    selectedRowRef.current?.scrollIntoView({ block: 'nearest' });
  }, [selectedRowKey]);
  const visibleRows = rows.slice(0, limit);
  const selectedOutsidePage =
    selectedRowKey && !visibleRows.some((row) => quantificationRowKey(row) === selectedRowKey)
      ? rows.find((row) => quantificationRowKey(row) === selectedRowKey)
      : undefined;
  if (selectedOutsidePage) visibleRows.push(selectedOutsidePage);
  const columnLabel = (column: string): string => {
    const labels: Record<string, string> = {
      peak_rt_start: 'Quantifier start RT',
      peak_rt_end: 'Quantifier end RT',
      qualifier_peak_rt_start: 'Qualifier start RT',
      qualifier_peak_rt_end: 'Qualifier end RT',
      internal_standard_peak_rt_start: 'IS start RT',
      internal_standard_peak_rt_end: 'IS end RT',
      calibration_range_status: 'Calibration range',
      calibration_level_status: 'Calibration level',
      qc_status: 'QC status',
      blank_status: 'Blank status',
      carryover_status: 'Carryover status',
      dilution_status: 'Dilution status',
      calibration_acceptance_status: 'Calibration acceptance',
      replicate_status: 'Replicate status',
      calibration_limit_min: 'Calibration limit min',
      calibration_limit_max: 'Calibration limit max',
      distinct_levels: 'Distinct levels',
      min_replicates_per_level: 'Min reps / level',
      max_replicates_per_level: 'Max reps / level',
      candidate_peak_ids: 'Candidate peak IDs',
      qualifier_candidate_peak_ids: 'Qualifier candidate IDs',
    };
    return labels[column] ?? (column === 'eic'
      ? 'Quantifier EIC'
      : column === 'qualifier_eic'
        ? 'Qualifier EIC'
        : column === 'internal_standard_eic'
          ? 'Internal standard EIC'
          : column.replaceAll('_', ' '));
  };
  return (
    <section className={`sf-quantification-table-card${fillHeight ? ' sf-quantification-table-card--fill' : ''}`}>
      {showHeader && (
        <h3>
          {title} <small>{rows.length} rows</small>
        </h3>
      )}
      <div className="sf-quantification-table-scroll">
        <table>
          <thead>
            <tr>
              {columns.map((column) => (
                <th key={column}>
                  {columnLabel(column)}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {visibleRows.map((row, index) => {
              const key = quantificationRowKey(row);
              const selected = key === selectedRowKey;
              return (
                <tr
                  key={`${key}-${index}`}
                  ref={(element) => {
                    if (selected) selectedRowRef.current = element;
                  }}
                  className={selected ? 'is-selected' : undefined}
                  aria-current={selected ? 'true' : undefined}
                  onClick={() => onRowSelect?.(key)}
                  onDoubleClick={() => onRowOpen?.(row)}
                  title="Double-click to inspect this row"
                >
                  {columns.map((column) => {
                    if (column === 'eic' || column === 'qualifier_eic' || column === 'internal_standard_eic') {
                      const prefix = column;
                      const label =
                        column === 'eic'
                          ? 'Quantifier EIC'
                          : column === 'qualifier_eic'
                            ? 'Qualifier EIC'
                            : 'Internal standard EIC';
                      return (
                        <td key={column}>
                          <EicSparkline row={row} prefix={prefix} label={label} />
                        </td>
                      );
                    }
                    const value = column === 'accuracy_check' ? accuracyStatus(row, accuracyLimit) : text(row, column);
                    const isStatus = [
                      'status',
                      'fit_status',
                      'quantifier_status',
                      'internal_standard_status',
                      'qualifier_status',
                      'accuracy_check',
                      'calibration_range_status',
                      'calibration_level_status',
                      'qc_status',
                      'blank_status',
                      'carryover_status',
                      'dilution_status',
                      'calibration_acceptance_status',
                      'replicate_status',
                    ].includes(column);
                    return (
                      <td key={column} className={isStatus ? statusClass(value) : undefined}>
                        {isStatus ? (
                          <span className="sf-quantification-status">{value || '—'}</span>
                        ) : (
                          format(value, column.includes('rt') ? 3 : 4)
                        )}
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      {rows.length > limit && <p className="sf-quantification-note">Showing the first {limit} rows.</p>}
    </section>
  );
}

type DetailMetric = { label: string; column: string; digits?: number };

const ROW_DETAIL_METRICS: DetailMetric[] = [
  { label: 'Sample type', column: 'sample_type' },
  { label: 'Role', column: 'role' },
  { label: 'Nominal concentration', column: 'nominal_concentration' },
  { label: 'Calibration level', column: 'calibration_level', digits: 0 },
  { label: 'Dilution factor', column: 'dilution_factor' },
  { label: 'Normalization', column: 'normalization' },
  { label: 'Internal standard ID', column: 'internal_standard_id' },
  { label: 'Response metric', column: 'response_metric' },
  { label: 'Response', column: 'response' },
  { label: 'Internal standard response', column: 'internal_standard_response' },
  { label: 'Response ratio', column: 'response_ratio' },
  { label: 'Qualifier response', column: 'qualifier_response' },
  { label: 'Qualifier ratio', column: 'qualifier_ratio' },
  { label: 'Status', column: 'status' },
  { label: 'Quantifier status', column: 'quantifier_status' },
  { label: 'Qualifier status', column: 'qualifier_status' },
  { label: 'Internal standard status', column: 'internal_standard_status' },
  { label: 'Candidate count', column: 'candidate_count', digits: 0 },
  { label: 'Qualifier candidate count', column: 'qualifier_candidate_count', digits: 0 },
  { label: 'Candidate peak IDs', column: 'candidate_peak_ids' },
  { label: 'Qualifier candidate peak IDs', column: 'qualifier_candidate_peak_ids' },
  { label: 'Included', column: 'included' },
  { label: 'Back-calculated concentration', column: 'back_calculated_concentration' },
  { label: 'Residual', column: 'residual' },
  { label: 'Relative residual', column: 'relative_residual' },
  { label: 'Exclusion reason', column: 'exclusion_reason' },
  { label: 'Quantifier RT error', column: 'rt_error', digits: 3 },
  { label: 'Qualifier RT error', column: 'qualifier_rt_error', digits: 3 },
  { label: 'Calculated concentration', column: 'calculated_concentration' },
  { label: 'Final concentration', column: 'final_concentration' },
  { label: 'Concentration unit', column: 'concentration_unit' },
  { label: 'Qualifier ratio pass', column: 'qualifier_ratio_pass' },
  { label: 'Accuracy percent', column: 'accuracy_percent' },
  { label: 'Flags', column: 'flags' },
  { label: 'Calibration range status', column: 'calibration_range_status' },
  { label: 'Calibration level status', column: 'calibration_level_status' },
  { label: 'QC status', column: 'qc_status' },
  { label: 'Blank status', column: 'blank_status' },
  { label: 'Carryover status', column: 'carryover_status' },
  { label: 'Dilution status', column: 'dilution_status' },
];

function DetailMetricList({ metrics, row }: { metrics: DetailMetric[]; row: QuantRow }) {
  const available = metrics.filter(({ column }) => text(row, column) !== '');
  if (!available.length) return <p className="sf-quantification-detail-empty">No scalar metrics available.</p>;
  return (
    <dl className="sf-quantification-detail-metrics">
      {available.map(({ label, column, digits = 4 }) => (
        <div key={column}>
          <dt>{label}</dt>
          <dd>{format(text(row, column), digits)}</dd>
        </div>
      ))}
    </dl>
  );
}

function PeakDetailCard({
  row,
  prefix,
  label,
  metrics,
}: {
  row: QuantRow;
  prefix: string;
  label: string;
  metrics: DetailMetric[];
}) {
  return (
    <section className="sf-quantification-detail-peak-card">
      <h3>{label}</h3>
      <EicDetailPlot row={row} prefix={prefix} label={label} />
      <DetailMetricList metrics={metrics} row={row} />
    </section>
  );
}

function QuantificationRowDetailModal({ row, onClose }: { row: QuantRow; onClose: () => void }) {
  const quantifierMetrics: DetailMetric[] = [
    { label: 'Chromatogram ID', column: 'quantifier_chromatogram_id' },
    { label: 'Chromatogram index', column: 'chromatogram_index', digits: 0 },
    { label: 'Peak ID', column: 'peak_id', digits: 0 },
    { label: 'Peak RT', column: 'peak_rt', digits: 3 },
    { label: 'Integration start RT', column: 'peak_rt_start', digits: 3 },
    { label: 'Integration end RT', column: 'peak_rt_end', digits: 3 },
    { label: 'Peak area', column: 'peak_area' },
    { label: 'Peak height', column: 'peak_height' },
  ];
  const qualifierMetrics: DetailMetric[] = [
    { label: 'Chromatogram ID', column: 'qualifier_chromatogram_id' },
    { label: 'Chromatogram index', column: 'qualifier_chromatogram_index', digits: 0 },
    { label: 'Peak ID', column: 'qualifier_peak_id', digits: 0 },
    { label: 'Peak RT', column: 'qualifier_peak_rt', digits: 3 },
    { label: 'Integration start RT', column: 'qualifier_peak_rt_start', digits: 3 },
    { label: 'Integration end RT', column: 'qualifier_peak_rt_end', digits: 3 },
    { label: 'Peak area', column: 'qualifier_peak_area' },
    { label: 'Peak height', column: 'qualifier_peak_height' },
  ];
  const internalStandardMetrics: DetailMetric[] = [
    { label: 'Internal standard ID', column: 'internal_standard_id' },
    { label: 'Peak RT', column: 'internal_standard_peak_rt', digits: 3 },
    { label: 'Integration start RT', column: 'internal_standard_peak_rt_start', digits: 3 },
    { label: 'Integration end RT', column: 'internal_standard_peak_rt_end', digits: 3 },
    { label: 'Peak area', column: 'internal_standard_peak_area' },
    { label: 'Peak height', column: 'internal_standard_peak_height' },
    { label: 'Response', column: 'internal_standard_response' },
    { label: 'Status', column: 'internal_standard_status' },
  ];
  return (
    <div
      className="sf-quantification-detail-backdrop"
      role="presentation"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <section className="sf-quantification-detail-modal" role="dialog" aria-modal="true" aria-label="Quantification row details">
        <header className="sf-quantification-detail-header">
          <div>
            <h2>{text(row, 'target_id') || 'Quantification row'}</h2>
            <p className="sf-quantification-detail-subtitle">
              {text(row, 'analysis') || '—'} · {text(row, 'sample_type') || '—'}
            </p>
          </div>
          <button type="button" className="sf-quantification-detail-close" onClick={onClose} aria-label="Close row details">
            ×
          </button>
        </header>
        <div className="sf-quantification-detail-body">
          <div className="sf-quantification-detail-peak-grid">
            <PeakDetailCard row={row} prefix="eic" label="Quantifier EIC" metrics={quantifierMetrics} />
            <PeakDetailCard row={row} prefix="qualifier_eic" label="Qualifier EIC" metrics={qualifierMetrics} />
            <PeakDetailCard
              row={row}
              prefix="internal_standard_eic"
              label="Internal standard EIC"
              metrics={internalStandardMetrics}
            />
          </div>
          <section className="sf-quantification-detail-section">
            <h3>Quantification metrics</h3>
            <DetailMetricList metrics={ROW_DETAIL_METRICS} row={row} />
          </section>
        </div>
      </section>
    </div>
  );
}

export function QuantifySamplesViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [tables, setTables] = useState<TableRows>({});
  const [loadState, setLoadState] = useState<{ resultId: string; error?: string } | null>(null);
  const [tab, setTab] = useState<Tab>('calibration');
  const [filters, setFilters] = useState<MassSpecFilterValues>({});
  const [metric, setMetric] = useState<Metric>('final_concentration');
  const [accuracyLimit, setAccuracyLimit] = useState(20);
  const [selectedRowKey, setSelectedRowKey] = useState<string | null>(null);
  const [detailRow, setDetailRow] = useState<QuantRow | null>(null);
  const [loadNotice, setLoadNotice] = useState('');
  const resultId = artifact?.result_id ?? '';
  const loading = !loadState || loadState.resultId !== resultId;
  const error = loadState?.resultId === resultId ? loadState.error : undefined;
  useEffect(() => {
    if (!detailRow) return undefined;
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setDetailRow(null);
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => window.removeEventListener('keydown', closeOnEscape);
  }, [detailRow]);
  useEffect(() => {
    if (!artifact || !client) return undefined;
    let cancelled = false;
    void client
      .currentResults(context.sessionId)
      .then(async (inventory) => {
        const siblings = inventory.filter(
          (candidate) =>
            candidate.producer_instance === artifact.producer_instance &&
            CONTRACTS.includes(localArtifactContract(candidate.contract_id) as (typeof CONTRACTS)[number]),
        );
        if (!siblings.some((candidate) => candidate.result_id === artifact.result_id)) siblings.push(artifact);
        const loaded = await Promise.all(
          siblings.map(async (candidate) => {
            const contract = localArtifactContract(candidate.contract_id);
            const columns =
              contract === 'quantificationCalibrationModelsTable' ? MODEL_COLUMNS : QUANTIFICATION_QUERY_COLUMNS;
            const rows: QuantRow[] = [];
            let offset = 0;
            let hasMore = false;
            do {
              const response = await client.resultQuery(context.sessionId, {
                result_id: candidate.result_id,
                mode: 'page',
                columns,
                offset,
                limit: 10000,
                ...(contract === 'quantificationTable' ? { eic_max_points: 256 } : {}),
              });
              rows.push(...response.rows);
              offset += response.rows.length;
              hasMore = response.has_more;
              if (!response.rows.length || rows.length >= 50000) break;
            } while (hasMore);
            return [contract, rows, hasMore] as const;
          }),
        );
        if (cancelled) return;
        setTables(Object.fromEntries(loaded.map(([contract, rows]) => [contract, rows])));
        const truncated = loaded.some(([, , hasMore]) => hasMore);
        setLoadNotice(truncated ? 'Only the first 50,000 rows are loaded; use filters before exporting very large results.' : '');
        setLoadState({ resultId });
      })
      .catch((reason: unknown) => {
        if (!cancelled)
          setLoadState({
            resultId,
            error: reason instanceof Error ? reason.message : 'Unable to load quantification results.',
          });
      });
    return () => {
      cancelled = true;
    };
  }, [artifact, client, context.sessionId, resultId]);

  const models = useMemo(() => tables.quantificationCalibrationModelsTable ?? [], [tables]);
  const quantification = useMemo(() => tables.quantificationTable ?? [], [tables]);
  const analyteQuantification = useMemo(
    () => quantification.filter((row) => text(row, 'role') === 'analyte'),
    [quantification],
  );
  const filterOptions = useMemo(
    () => ({
      target_id: [
        ...new Set([...models, ...analyteQuantification].map((row) => text(row, 'target_id')).filter(Boolean)),
      ],
      sample_type: [
        ...new Set(
          analyteQuantification
            .filter((row) =>
              tab === 'results' ? isDisplayedResultRow(row) : text(row, 'sample_type') === 'calibrator',
            )
            .map((row) => text(row, 'sample_type')),
        ),
      ].filter(Boolean),
    }),
    [analyteQuantification, models, tab],
  );
  const filteredQuantification = useMemo(
    () => quantification.filter((row) => matchesFilters(row, filters)),
    [filters, quantification],
  );
  const visibleModels = useMemo(
    () => models.filter((row) => matchesFilters(row, { target_id: filters.target_id ?? [] })),
    [filters.target_id, models],
  );
  const points = useMemo(
    () =>
      filteredQuantification.filter(
        (row) => text(row, 'role') === 'analyte' && text(row, 'sample_type') === 'calibrator',
      ),
    [filteredQuantification],
  );
  const resultRows = useMemo(() => filteredQuantification.filter(isDisplayedResultRow), [filteredQuantification]);
  const results = useMemo(() => resultRows.filter((row) => text(row, 'role') === 'analyte'), [resultRows]);
  const plotTheme = useMemo(() => theme(), []);
  const calibrationPlot = useMemo(
    () => calibrationSpec(visibleModels, points, '__all__', resultId, plotTheme),
    [points, plotTheme, resultId, visibleModels],
  );
  const residualPlot = useMemo(
    () => calibrationResidualSpec(visibleModels, points, '__all__', resultId, plotTheme),
    [points, plotTheme, resultId, visibleModels],
  );
  const resultPlot = useMemo(
    () => resultsSpec(results, visibleModels, '__all__', metric, accuracyLimit, resultId, plotTheme),
    [accuracyLimit, metric, plotTheme, resultId, results, visibleModels],
  );
  const handlePlotPointClick = (point: VisualizationPointClick) => {
    const key = plotRowKey(point.customdata);
    if (key) setSelectedRowKey(key);
  };

  if (!artifact || !client) return <div role="alert">Quantification result data is unavailable.</div>;
  if (loading) return <LoadingLogo label="Loading quantification results…" />;
  if (error)
    return (
      <div className="sf-quantification-viewer-state" role="alert">
        {error}
      </div>
    );
  return (
    <div className="sf-quantification-viewer">
      <div className="sf-quantification-viewer-toolbar" aria-label="Quantification viewer controls">
        <div className="sf-quantification-tabs" role="tablist" aria-label="Quantification views">
          <button
            className={tab === 'calibration' ? 'is-active' : ''}
            role="tab"
            aria-selected={tab === 'calibration'}
            onClick={() => setTab('calibration')}
          >
            Calibration
          </button>
          <button
            className={tab === 'results' ? 'is-active' : ''}
            role="tab"
            aria-selected={tab === 'results'}
            onClick={() => setTab('results')}
          >
            Results
          </button>
        </div>
        <MassSpecFilterToolbar
          showSelectByRow={false}
          selectByColumns={[]}
          filterColumns={[
            ['target_id', 'Compound'],
            ['sample_type', 'Sample type'],
          ]}
          selectBy={[]}
          onSelectByChange={() => undefined}
          optionValues={filterOptions}
          filters={filters}
          onFiltersChange={setFilters}
          summary={`${filteredQuantification.length.toLocaleString()} matching rows`}
        />
        {tab === 'results' && (
          <>
            <label>
              Plot{' '}
              <select value={metric} onChange={(event) => setMetric(event.target.value as Metric)}>
                <option value="final_concentration">Final concentration</option>
                <option value="response_ratio">Response ratio</option>
                <option value="response">Response</option>
              </select>
            </label>
            <label>
              Accuracy limit ±{' '}
              <input
                type="number"
                min="0"
                max="100"
                step="1"
                value={accuracyLimit}
                onChange={(event) => setAccuracyLimit(Number(event.target.value) || 0)}
              />{' '}
              %
            </label>
          </>
        )}
        {loadNotice && <span className="sf-quantification-load-notice">{loadNotice}</span>}
        <span className="sf-quantification-summary">
          {models.length} models · {quantification.length} quantification rows
        </span>
        <div className="sf-quantification-export" aria-label="Export quantification data">
          <button
            type="button"
            onClick={() => downloadRows(tab === 'calibration' ? 'quantification-calibration.csv' : 'quantification-results.csv', tab === 'calibration' ? points : results, tab === 'calibration' ? CALIBRATION_COLUMNS : QUANTIFICATION_COLUMNS, 'csv')}
          >
            Export CSV
          </button>
          <button
            type="button"
            onClick={() => downloadRows(tab === 'calibration' ? 'quantification-calibration.json' : 'quantification-results.json', tab === 'calibration' ? points : results, tab === 'calibration' ? CALIBRATION_COLUMNS : QUANTIFICATION_COLUMNS, 'json')}
          >
            Export JSON
          </button>
        </div>
      </div>
      {tab === 'calibration' ? (
        <>
          <div className="sf-quantification-calibration-top">
            <section>
              <div className="sf-quantification-plot">
                {calibrationPlot ? (
                  <VisualizationRenderer spec={calibrationPlot} onPointClick={handlePlotPointClick} />
                ) : (
                  <div className="sf-quantification-viewer-state">No calibration points are available.</div>
                )}
              </div>
            </section>
            <section>
              <div className="sf-quantification-plot">
                {residualPlot ? (
                  <VisualizationRenderer spec={residualPlot} onPointClick={handlePlotPointClick} />
                ) : (
                  <div className="sf-quantification-viewer-state">No residuals are available.</div>
                )}
              </div>
            </section>
            <DataTable title="Calibration models" rows={visibleModels} columns={MODEL_COLUMNS} showHeader={false} />
          </div>
          <DataTable
            title="Raw calibration points"
            rows={points}
            columns={CALIBRATION_COLUMNS}
            selectedRowKey={selectedRowKey}
            onRowSelect={setSelectedRowKey}
            onRowOpen={setDetailRow}
            showHeader={false}
            fillHeight
          />
        </>
      ) : (
        <>
          <div className="sf-quantification-plot sf-quantification-results-plot">
            {resultPlot ? (
              <VisualizationRenderer spec={resultPlot} onPointClick={handlePlotPointClick} />
            ) : (
              <div className="sf-quantification-viewer-state">No quantification results are available.</div>
            )}
          </div>
          <DataTable
            title="QC, unknown & blank results"
            rows={resultRows}
            columns={QUANTIFICATION_COLUMNS}
            selectedRowKey={selectedRowKey}
            onRowSelect={setSelectedRowKey}
            onRowOpen={setDetailRow}
            accuracyLimit={accuracyLimit}
            fillHeight
            showHeader={false}
          />
        </>
      )}
      {detailRow && <QuantificationRowDetailModal row={detailRow} onClose={() => setDetailRow(null)} />}
    </div>
  );
}
