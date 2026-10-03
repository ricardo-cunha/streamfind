import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath, URL } from 'node:url';
import { discoverPluginManifests } from './discover-plugin-manifests.mjs';

const root = fileURLToPath(new URL('../..', import.meta.url));
const pluginsRoot = join(root, 'src', 'plugins');
const dist = join(root, 'dist');
const viteManifestPath = join(dist, '.vite', 'manifest.json');
const outputPath = join(dist, 'plugins.json');
const viteManifest = JSON.parse(await readFile(viteManifestPath, 'utf8'));

const plugins = (await discoverPluginManifests(pluginsRoot)).map((registration) => {
  const { source } = registration;
  const manifest = { ...registration };
  delete manifest.activation;
  delete manifest.source;
  const entry = viteManifest[source]?.file;
  if (!entry) throw new Error(`Vite manifest did not emit plugin entry: ${source}`);
  return { ...manifest, entry: `/${entry}` };
});
await writeFile(outputPath, `${JSON.stringify({ plugins }, null, 2)}\n`);
console.log(`Wrote ${outputPath}`);
