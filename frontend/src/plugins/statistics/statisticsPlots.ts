import { localArtifactContract } from '../../framework/artifacts/artifactContracts';
import type { PlotlyTrace, VisualizationSpec } from '../../framework/visualization/visualizationTypes';

export const STATISTICS_TABLE_CONTRACTS = [
  'pcaScoresTable',
  'pcaLoadingsTable',
  'pcaVarianceTable',
  'plsScoresTable',
  'plsLoadingsTable',
  'plsCoefficientsTable',
  'plsPredictionsTable',
  'foldChangeTable',
] as const;

export const STATISTICS_RESULT_CONTRACTS = ['pcaDiagnostics', 'plsDiagnostics', 'foldChangeDiagnostics'] as const;

export const STATISTICS_ARTIFACT_CONTRACTS = [...STATISTICS_TABLE_CONTRACTS, ...STATISTICS_RESULT_CONTRACTS] as const;

export type StatisticsRow = Record<string, string | null>;
export type StatisticsRows = Partial<Record<(typeof STATISTICS_TABLE_CONTRACTS)[number], StatisticsRow[]>>;
export type StatisticsPlotTheme = { text: string; grid: string; background: string; accent: string };
export type StatisticsPlotPanel = {
  id: string;
  title: string;
  selectionKind: 'sample' | 'variable' | 'none';
  spec: VisualizationSpec;
};

const defaultTheme: StatisticsPlotTheme = {
  text: '#30343b',
  grid: '#d5d9df',
  background: '#ffffff',
  accent: '#3874c8',
};

function number(row: StatisticsRow, key: string): number {
  const value = Number(row[key]);
  return Number.isFinite(value) ? value : Number.NaN;
}

function text(row: StatisticsRow, key: string): string {
  return row[key] ?? '';
}

function colorForGroup(group: string, index: number, theme: StatisticsPlotTheme): string {
  if (!group) return theme.accent;
  return `hsl(${(index * 137.508 + 208) % 360} 62% 48%)`;
}

function makeSpec(
  id: string,
  title: string,
  xTitle: string,
  yTitle: string,
  traces: PlotlyTrace[],
  artifactId: string,
  theme: StatisticsPlotTheme,
): VisualizationSpec {
  return {
    schema: 'streamfind.visualization/v1',
    visualization_id: `statistics.${id}`,
    semantic_type: 'statistics.result',
    title,
    data_mode: 'inline',
    renderer: { engine: 'plotly', renderer_id: 'core.plotly', spec_version: '1' },
    payload: {
      data: traces,
      layout: {
        autosize: true,
        margin: { l: 62, r: 18, t: 14, b: 76 },
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        hoverlabel: { bgcolor: theme.background, font: { color: theme.text } },
        legend: {
          bgcolor: theme.background,
          font: { color: theme.text },
          orientation: 'h',
          x: 0,
          xanchor: 'left',
          y: -0.12,
          yanchor: 'top',
        },
        xaxis: { title: { text: xTitle }, showgrid: true, gridcolor: theme.grid, zerolinecolor: theme.grid },
        yaxis: { title: { text: yTitle }, showgrid: true, gridcolor: theme.grid, zerolinecolor: theme.grid },
        hovermode: 'closest',
      },
      config: { responsive: true, displaylogo: false },
    },
    provenance: {
      source_artifact_ids: artifactId ? [artifactId] : ['statistics-dashboard'],
      producer_operation_id: 'frontend.statistics.dashboard',
      producer_node_id: 'frontend.statistics.dashboard',
    },
    fallback: { description: `${title} plot is unavailable.` },
  };
}

function panel(
  id: string,
  title: string,
  selectionKind: StatisticsPlotPanel['selectionKind'],
  spec: VisualizationSpec,
): StatisticsPlotPanel {
  return { id, title, selectionKind, spec };
}

function scoreBySample(rows: StatisticsRow[], scoreColumn: string): StatisticsRow[] {
  const samples = new Map<string, StatisticsRow>();
  for (const row of rows) {
    const id = text(row, 'sample_id');
    if (!id) continue;
    const sample = samples.get(id) ?? { sample_id: id, group_label: row.group_label };
    sample[`component_${row.component}`] = row[scoreColumn] ?? null;
    samples.set(id, sample);
  }
  return [...samples.values()];
}

function groupedScatter(
  rows: StatisticsRow[],
  xKey: string,
  yKey: string,
  selectionKey: string,
  selectedId: string | null,
  theme: StatisticsPlotTheme,
): PlotlyTrace[] {
  const groups = new Map<string, StatisticsRow[]>();
  for (const row of rows) {
    if (!Number.isFinite(number(row, xKey)) || !Number.isFinite(number(row, yKey))) continue;
    const group = text(row, 'group_label') || 'samples';
    const values = groups.get(group) ?? [];
    values.push(row);
    groups.set(group, values);
  }
  return [...groups.entries()].map(([group, points], index) => ({
    type: 'scattergl',
    mode: 'markers',
    name: group,
    x: points.map((row) => number(row, xKey)),
    y: points.map((row) => number(row, yKey)),
    customdata: points.map((row) => text(row, selectionKey)),
    marker: {
      color: colorForGroup(group, index, theme),
      size: points.map((row) => (text(row, selectionKey) === selectedId ? 13 : 8)),
      line: { color: theme.background, width: points.map((row) => (text(row, selectionKey) === selectedId ? 2 : 0)) },
    },
    hovertemplate: `${selectionKey}: %{customdata}<br>${xKey}: %{x}<br>${yKey}: %{y}<extra>${group}</extra>`,
  }));
}

export function buildStatisticsPanels(
  rows: StatisticsRows,
  artifactId: string,
  theme: Partial<StatisticsPlotTheme> = {},
  selectedId: string | null = null,
): StatisticsPlotPanel[] {
  const plotTheme = { ...defaultTheme, ...theme };
  const panels: StatisticsPlotPanel[] = [];
  const pcaScores = rows.pcaScoresTable ?? [];
  if (pcaScores.length) {
    const pivoted = scoreBySample(pcaScores, 'score');
    panels.push(
      panel(
        'pca-scores',
        'PCA scores',
        'sample',
        makeSpec(
          'pca-scores',
          'PCA scores',
          'PC 1 score',
          'PC 2 score',
          groupedScatter(
            pivoted.map((row) => ({
              ...row,
              component_1: row.component_1 ?? '0',
              component_2: row.component_2 ?? '0',
            })),
            'component_1',
            'component_2',
            'sample_id',
            selectedId,
            plotTheme,
          ),
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  const pcaLoadings = rows.pcaLoadingsTable ?? [];
  if (pcaLoadings.length) {
    const pivoted = new Map<string, StatisticsRow>();
    for (const row of pcaLoadings) {
      const id = text(row, 'variable_id');
      const item = pivoted.get(id) ?? { variable_id: id };
      item[`component_${row.component}`] = row.loading;
      pivoted.set(id, item);
    }
    const points = [...pivoted.values()].filter((row) => Number.isFinite(number(row, 'component_1')));
    panels.push(
      panel(
        'pca-loadings',
        'PCA loadings',
        'variable',
        makeSpec(
          'pca-loadings',
          'PCA loadings',
          'PC 1 loading',
          'PC 2 loading',
          [
            {
              type: 'scattergl',
              mode: 'markers+text',
              text: points.map((row) => text(row, 'variable_id')),
              textposition: 'top center',
              x: points.map((row) => number(row, 'component_1')),
              y: points.map((row) => (Number.isFinite(number(row, 'component_2')) ? number(row, 'component_2') : 0)),
              customdata: points.map((row) => text(row, 'variable_id')),
              marker: {
                color: points.map((row) => (text(row, 'variable_id') === selectedId ? '#e45756' : plotTheme.accent)),
                size: 9,
              },
              hovertemplate: 'Variable: %{customdata}<br>PC 1: %{x}<br>PC 2: %{y}<extra></extra>',
            },
          ],
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  const variance = rows.pcaVarianceTable ?? [];
  if (variance.length) {
    panels.push(
      panel(
        'pca-variance',
        'Explained variance',
        'none',
        makeSpec(
          'pca-variance',
          'Explained variance',
          'Component',
          'Explained variance',
          [
            {
              type: 'bar',
              x: variance.map((row) => `PC ${row.component}`),
              y: variance.map((row) => number(row, 'explained_variance_ratio')),
              customdata: variance.map((row) => row.component ?? ''),
              marker: { color: plotTheme.accent },
              hovertemplate: '%{x}<br>Explained variance: %{y:.1%}<extra></extra>',
            },
          ],
          artifactId,
          plotTheme,
        ),
      ),
    );
  }

  const plsScores = rows.plsScoresTable ?? [];
  if (plsScores.length) {
    const componentOne = plsScores.filter((row) => Number(row.component) === 1);
    panels.push(
      panel(
        'pls-scores',
        'PLS score relationship',
        'sample',
        makeSpec(
          'pls-scores',
          'PLS score relationship',
          'X score',
          'Y score',
          groupedScatter(componentOne, 'x_score', 'y_score', 'sample_id', selectedId, plotTheme),
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  const predictions = rows.plsPredictionsTable ?? [];
  if (predictions.length) {
    const numeric = predictions.filter(
      (row) => Number.isFinite(number(row, 'observed_value')) && Number.isFinite(number(row, 'predicted_value')),
    );
    if (numeric.length) {
      panels.push(
        panel(
          'pls-predictions',
          'PLS fitted predictions',
          'sample',
          makeSpec(
            'pls-predictions',
            'PLS fitted predictions',
            'Observed',
            'Predicted',
            [
              {
                type: 'scattergl',
                mode: 'markers',
                name: 'In-sample fit',
                x: numeric.map((row) => number(row, 'observed_value')),
                y: numeric.map((row) => number(row, 'predicted_value')),
                customdata: numeric.map((row) => text(row, 'sample_id')),
                marker: {
                  color: numeric.map((row) => (text(row, 'sample_id') === selectedId ? '#e45756' : plotTheme.accent)),
                  size: 9,
                },
                hovertemplate: 'Sample: %{customdata}<br>Observed: %{x}<br>Predicted: %{y}<extra></extra>',
              },
            ],
            artifactId,
            plotTheme,
          ),
        ),
      );
    } else {
      panels.push(
        panel(
          'pls-predictions',
          'PLS-DA fitted predictions',
          'sample',
          makeSpec(
            'pls-predictions',
            'PLS-DA fitted predictions',
            'Sample',
            'Class confidence',
            [
              {
                type: 'scattergl',
                mode: 'markers',
                name: 'In-sample class score',
                x: predictions.map((_, index) => index + 1),
                y: predictions.map((row) => number(row, 'confidence')),
                customdata: predictions.map((row) => [
                  text(row, 'sample_id'),
                  text(row, 'predicted_label'),
                  text(row, 'observed_label'),
                ]),
                marker: {
                  color: predictions.map((row) =>
                    text(row, 'sample_id') === selectedId ? '#e45756' : plotTheme.accent,
                  ),
                  size: 9,
                },
                hovertemplate:
                  'Sample: %{customdata[0]}<br>Observed: %{customdata[2]}<br>Predicted: %{customdata[1]}<br>Score: %{y}<extra></extra>',
              },
            ],
            artifactId,
            plotTheme,
          ),
        ),
      );
    }
  }
  const plsLoadings = (rows.plsLoadingsTable ?? []).filter((row) => Number(row.component) === 1);
  if (plsLoadings.length) {
    panels.push(
      panel(
        'pls-loadings',
        'PLS component 1 loadings',
        'variable',
        makeSpec(
          'pls-loadings',
          'PLS component 1 loadings',
          'Block item',
          'Loading',
          [
            {
              type: 'bar',
              x: plsLoadings.map((row) => `${text(row, 'block')}:${text(row, 'item_id')}`),
              y: plsLoadings.map((row) => number(row, 'loading')),
              customdata: plsLoadings.map((row) => text(row, 'item_id')),
              marker: {
                color: plsLoadings.map((row) => (text(row, 'item_id') === selectedId ? '#e45756' : plotTheme.accent)),
              },
              hovertemplate: 'Item: %{customdata}<br>Loading: %{y}<extra></extra>',
            },
          ],
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  const coefficients = rows.plsCoefficientsTable ?? [];
  if (coefficients.length) {
    const response = text(coefficients[0], 'response_id');
    const filtered = coefficients.filter((row) => text(row, 'response_id') === response);
    panels.push(
      panel(
        'pls-coefficients',
        'PLS coefficients',
        'variable',
        makeSpec(
          'pls-coefficients',
          'PLS coefficients',
          'Predictor',
          'Coefficient',
          [
            {
              type: 'bar',
              x: filtered.map((row) => text(row, 'predictor_id')),
              y: filtered.map((row) => number(row, 'coefficient')),
              customdata: filtered.map((row) => text(row, 'predictor_id')),
              marker: {
                color: filtered.map((row) => (text(row, 'predictor_id') === selectedId ? '#e45756' : plotTheme.accent)),
              },
              hovertemplate: 'Predictor: %{customdata}<br>Coefficient: %{y}<extra></extra>',
            },
          ],
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  const foldChange = rows.foldChangeTable ?? [];
  if (foldChange.length) {
    const points = foldChange.filter(
      (row) => Number.isFinite(number(row, 'log2_fold_change')) && Number.isFinite(number(row, 'p_value')),
    );
    panels.push(
      panel(
        'fold-change-volcano',
        'Fold-change volcano plot',
        'variable',
        makeSpec(
          'fold-change-volcano',
          'Fold-change volcano plot',
          'log2 fold change',
          '−log10(p-value)',
          [
            {
              type: 'scattergl',
              mode: 'markers',
              name: 'Variables',
              x: points.map((row) => number(row, 'log2_fold_change')),
              y: points.map((row) => -Math.log10(Math.max(number(row, 'p_value'), 1e-300))),
              customdata: points.map((row) => text(row, 'variable_id')),
              marker: {
                color: points.map((row) =>
                  text(row, 'variable_id') === selectedId
                    ? '#e45756'
                    : row.significant === 'true'
                      ? '#c44755'
                      : plotTheme.accent,
                ),
                size: points.map((row) => (text(row, 'variable_id') === selectedId ? 13 : 8)),
              },
              hovertemplate: 'Variable: %{customdata}<br>log2 fold change: %{x}<br>−log10(p): %{y}<extra></extra>',
            },
          ],
          artifactId,
          plotTheme,
        ),
      ),
    );
  }
  return panels;
}

export function statisticsContract(value: string): string {
  return localArtifactContract(value);
}

export function getStatisticsSelection(point: { customdata?: unknown }): string | null {
  const raw = point.customdata;
  const value = Array.isArray(raw) ? raw[0] : raw;
  return typeof value === 'string' && value.length ? value : null;
}
