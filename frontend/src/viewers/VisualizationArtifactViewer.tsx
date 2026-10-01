import { useEffect, useMemo, useState, type ReactNode } from 'react';
import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import { VisualizationRenderer } from '../visualization/VisualizationRenderer';
import { visualizationSpecFromArtifact } from '../visualization/VisualizationDataResolver';
import type { VisualizationSpec } from '../visualization/visualizationTypes';
import type { ViewerComponentProps } from './viewerTypes';

export function VisualizationArtifactViewer({ context }: ViewerComponentProps): ReactNode {
  const [darkMode, setDarkMode] = useState(() => document.documentElement.dataset.theme === 'dark');
  useEffect(() => {
    const observer = new MutationObserver(() => setDarkMode(document.documentElement.dataset.theme === 'dark'));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    return () => observer.disconnect();
  }, []);

  const spec = context.artifact ? parseSpec(context.artifact) : undefined;
  const themedSpec = useMemo(() => (spec ? themeTicSpec(spec, darkMode) : undefined), [darkMode, spec]);
  if (!context.artifact) return <div role="alert">Visualization artifact data is unavailable.</div>;
  if (!spec) {
    return <pre className="sf-artifact-viewer-json">{prettyPayload(context.artifact.payload)}</pre>;
  }
  return <VisualizationRenderer spec={themedSpec ?? spec} className="sf-visualization-preview" />;
}

function themeTicSpec(spec: VisualizationSpec, darkMode: boolean): VisualizationSpec {
  if (spec.semantic_type !== 'mass_spec.spectra_tic' && !spec.visualization_id.includes('plot_spectra_tic'))
    return spec;
  const theme = getPlotTheme(darkMode);
  const categories = Array.from(new Set(spec.payload.data.map((trace) => String(trace.name ?? 'trace')))).sort();
  return {
    ...spec,
    payload: {
      ...spec.payload,
      data: spec.payload.data.map((trace) => {
        const color = colorFor(String(trace.name ?? 'trace'), categories);
        return {
          ...trace,
          line: { ...(isRecord(trace.line) ? trace.line : {}), color, width: 2 },
          marker: { ...(isRecord(trace.marker) ? trace.marker : {}), color },
        };
      }),
      layout: {
        ...(spec.payload.layout ?? {}),
        autosize: true,
        margin: { l: 50, r: 30, t: 30, b: 50 },
        xaxis: {
          ...(isRecord(spec.payload.layout?.xaxis) ? spec.payload.layout.xaxis : {}),
          title: { text: 'Retention time (s)' },
          showgrid: true,
          zeroline: false,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
        },
        yaxis: {
          ...(isRecord(spec.payload.layout?.yaxis) ? spec.payload.layout.yaxis : {}),
          title: { text: 'Intensity' },
          showgrid: true,
          zeroline: false,
          gridcolor: theme.grid,
          linecolor: theme.text,
          tickfont: { color: theme.text },
          titlefont: { color: theme.text },
        },
        paper_bgcolor: theme.background,
        plot_bgcolor: theme.background,
        font: { color: theme.text },
        hoverlabel: { bgcolor: theme.background, font: { color: theme.text } },
      },
    },
  };
}

const plotPalette = [
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

function colorFor(value: string, categories: string[]): string {
  return plotPalette[Math.max(0, categories.indexOf(value)) % plotPalette.length];
}

function isRecord(value: unknown): value is Record<string, never> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function getPlotTheme(darkMode: boolean): { text: string; grid: string; background: string } {
  const modalBackground = getComputedStyle(document.documentElement).getPropertyValue('--sf-surface-raised').trim();
  const background = modalBackground || (darkMode ? '#132226' : '#ffffff');
  return darkMode
    ? { text: '#ffffff', grid: 'rgba(255, 255, 255, 0.16)', background }
    : { text: '#000000', grid: 'rgba(0, 0, 0, 0.14)', background };
}

function parseSpec(artifact: ArtifactRecord) {
  try {
    return visualizationSpecFromArtifact(artifact);
  } catch {
    return undefined;
  }
}

function prettyPayload(payload: ArtifactRecord['payload']): string {
  if (typeof payload !== 'string') return JSON.stringify(payload ?? null, null, 2);
  try {
    return JSON.stringify(JSON.parse(payload), null, 2);
  } catch {
    return payload;
  }
}
