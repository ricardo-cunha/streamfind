import { useEffect, useState } from 'react';
import { VisualizationRenderer } from './visualization/VisualizationRenderer';
import { parseVisualizationSpec } from './visualization/visualizationTypes';

declare global {
  interface Window {
    __MCP_STRUCTURED_CONTENT__?: unknown;
  }
}

function incomingVisualization(event: MessageEvent): unknown {
  if (event.data && typeof event.data === 'object' && 'structuredContent' in event.data)
    return event.data.structuredContent;
  return event.data;
}

export function McpVisualizationApp() {
  const [spec, setSpec] = useState(() => parseVisualizationSpec(window.__MCP_STRUCTURED_CONTENT__));

  useEffect(() => {
    const receive = (event: MessageEvent) => {
      const next = parseVisualizationSpec(incomingVisualization(event));
      if (next) setSpec(next);
    };
    window.addEventListener('message', receive);
    return () => window.removeEventListener('message', receive);
  }, []);

  if (!spec) return <main className="sf-mcp-visualization-empty">Waiting for a StreamFind visualization result.</main>;

  return <VisualizationRenderer spec={spec} className="sf-mcp-visualization" />;
}
