import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './app/App';
import { registerCoreVisualizationRenderers } from './visualization/registerVisualizationRenderers';
import './theme/theme.css';

async function bootstrap() {
  const { default: Plotly } = await import('plotly.js-dist-min');
  registerCoreVisualizationRenderers(Plotly);
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}

void bootstrap();
