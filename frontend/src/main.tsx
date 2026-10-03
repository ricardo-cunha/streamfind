import { StrictMode, type ReactNode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './application/App';
import { StartupFailureScreen } from './application/StartupFailureScreen';
import { registerCoreVisualizationRenderers } from './plugins/core/visualization/registerVisualizationRenderers';
import { registerCoreFrontendPlugin } from './plugins/core';
import { frontendPluginRegistry } from './framework/plugins/FrontendPluginRegistry';
import { FrontendPluginLoader } from './framework/plugins/FrontendPluginLoader';
import { retainBundledPluginChunks } from './framework/plugins/bundledPluginEntries';
import './framework/theme/theme.css';

function mount(node: ReactNode): void {
  const root = document.getElementById('root');
  if (!root) throw new Error('Application root element was not found');
  createRoot(root).render(<StrictMode>{node}</StrictMode>);
}

async function bootstrap(): Promise<void> {
  const { default: Plotly } = await import('plotly.js-dist-min');
  registerCoreVisualizationRenderers({
    newPlot: Plotly.newPlot,
    purge: Plotly.purge,
    resize: Plotly.Plots.resize,
  });
  registerCoreFrontendPlugin();
  retainBundledPluginChunks();
  await new FrontendPluginLoader(frontendPluginRegistry).loadManifestUrl();
  mount(<App />);
}

void bootstrap().catch((error: unknown) => {
  mount(<StartupFailureScreen error={error} />);
});
