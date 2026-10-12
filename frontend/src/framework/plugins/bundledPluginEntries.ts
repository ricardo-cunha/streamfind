// Keep plugin entry modules in Vite's production graph without naming domains here.
export const bundledFrontendPluginEntries = import.meta.glob([
  '../../plugins/*/index.ts',
  '!../../plugins/core/index.ts',
]);

export function retainBundledPluginChunks(): void {
  const globals = globalThis as typeof globalThis & { __streamfindPreloadPlugins?: boolean };
  if (!globals.__streamfindPreloadPlugins) return;
  for (const loadPlugin of Object.values(bundledFrontendPluginEntries)) void loadPlugin();
}
