import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { McpVisualizationApp } from './McpVisualizationApp';
import { registerCoreVisualizationRenderers } from './visualization/registerVisualizationRenderers';
import './theme/theme.css';

async function bootstrap() {
  const { default: Plotly } = await import('plotly.js-dist-min');
  registerCoreVisualizationRenderers(Plotly);
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <McpVisualizationApp />
    </StrictMode>,
  );
}

void bootstrap();
