import * as ReactRuntime from 'react';
import * as ReactDomRuntime from 'react-dom';
import * as ReactDomClientRuntime from 'react-dom/client';
import * as ReactJsxRuntime from 'react/jsx-runtime';
import { StrictMode, type ReactNode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './application/App';
import { StartupFailureScreen } from './application/StartupFailureScreen';
import { registerCoreVisualizationRenderers } from './plugins/core/visualization/registerVisualizationRenderers';
import { registerCoreFrontendPlugin } from './plugins/core';
import { frontendPluginRegistry } from './framework/plugins/FrontendPluginRegistry';
import { FrontendPluginLoader } from './framework/plugins/FrontendPluginLoader';
import { retainBundledPluginChunks } from './framework/plugins/bundledPluginEntries';
import * as ViewerSdkRuntime from './framework/viewer-sdk';
import './framework/theme/theme.css';

const runtimeGlobal = globalThis as typeof globalThis & {
  __streamfindReact?: typeof ReactRuntime;
  __streamfindReactDom?: typeof ReactDomRuntime;
  __streamfindReactDomClient?: typeof ReactDomClientRuntime;
  __streamfindJsxRuntime?: typeof ReactJsxRuntime;
  __streamfindViewerSdk?: typeof ViewerSdkRuntime;
};

runtimeGlobal.__streamfindReact = ReactRuntime;
runtimeGlobal.__streamfindReactDom = ReactDomRuntime;
runtimeGlobal.__streamfindReactDomClient = ReactDomClientRuntime;
runtimeGlobal.__streamfindJsxRuntime = ReactJsxRuntime;
runtimeGlobal.__streamfindViewerSdk = ViewerSdkRuntime;

function mount(node: ReactNode): void {
  const root = document.getElementById('root');
  if (!root) throw new Error('Application root element was not found');
  createRoot(root).render(<StrictMode>{node}</StrictMode>);
}

async function bootstrap(): Promise<void> {
  const { default: Plotly } = await import('plotly.js-dist-min');
  registerCoreVisualizationRenderers({
    newPlot: Plotly.newPlot,
    react: (Plotly as unknown as { react: typeof Plotly.newPlot }).react,
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
