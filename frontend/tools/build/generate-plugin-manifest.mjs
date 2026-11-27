import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath, URL } from 'node:url';
import { discoverPluginManifests } from './discover-plugin-manifests.mjs';

const root = fileURLToPath(new URL('../..', import.meta.url));
const pluginsRoot = join(root, 'src', 'plugins');
const dist = join(root, 'dist');
const viteManifestPath = join(dist, '.vite', 'manifest.json');
const outputPath = join(dist, 'plugins.json');
const viteManifest = JSON.parse(await readFile(viteManifestPath, 'utf8'));

function collectCssAssets(source) {
  const visited = new Set();
  const css = new Set();

  function visit(key) {
    if (visited.has(key)) return;
    visited.add(key);

    const entry = viteManifest[key];
    if (!entry) throw new Error(`Vite manifest did not emit imported plugin chunk: ${key}`);
    for (const asset of entry.css ?? []) css.add(asset);
    for (const imported of entry.imports ?? []) visit(imported);
  }

  visit(source);
  return [...css];
}

const registrations = await discoverPluginManifests(pluginsRoot);
const plugins = registrations.map((registration) => {
  const { source } = registration;
  const manifest = { ...registration };
  delete manifest.activation;
  delete manifest.source;
  const viteEntry = viteManifest[source];
  const entry = viteEntry?.file;
  if (!entry) throw new Error(`Vite manifest did not emit plugin entry: ${source}`);
  const css = viteEntry.css?.map((asset) => `/${asset}`) ?? [];
  return { ...manifest, entry: `/${entry}`, ...(css.length ? { css } : {}) };
});

const nativeViewerRoot = join(dist, 'viewers', 'native');
await rm(nativeViewerRoot, { recursive: true, force: true });
await mkdir(nativeViewerRoot, { recursive: true });
const sharedAssets = join(nativeViewerRoot, '_shared', 'assets');
await mkdir(sharedAssets, { recursive: true });
await cp(join(dist, 'assets'), sharedAssets, { recursive: true });
for (const registration of registrations) {
  const viteEntry = viteManifest[registration.source];
  if (!viteEntry?.file) throw new Error(`Vite manifest did not emit viewer entry: ${registration.source}`);
  const packageRoot = join(nativeViewerRoot, registration.id);
  const packageAssets = join(packageRoot, 'assets');
  await mkdir(packageAssets, { recursive: true });
  const entryAsset = viteEntry.file.split('/').pop();
  await writeFile(
    join(packageAssets, 'viewer.mjs'),
    // Load the viewer through the host asset URL. The viewer package also
    // keeps a _shared copy for layout/distribution, but using /assets here is
    // essential: byte-identical modules at two URLs create duplicate React
    // and ReactDOM instances in the browser.
    `export { default } from '/assets/${entryAsset}';\nexport * from '/assets/${entryAsset}';\n`,
  );
  // Vite attaches extracted CSS to the shared chunk that imports it, rather
  // than to every viewer entry. Include styles from the complete static import
  // graph so dynamically loaded viewers receive their own stylesheets.
  const css = collectCssAssets(registration.source).map((asset) => asset.split('/').pop());
  if (css.length) {
    await writeFile(
      join(packageAssets, 'viewer.css'),
      `${css.map((asset) => `@import url('/assets/${asset}');`).join('\n')}\n`,
    );
  }
  const packageManifest = {
    ...registration,
    entry: 'assets/viewer.mjs',
    ...(css.length ? { css: ['assets/viewer.css'] } : {}),
    route: '/viewers/',
    active: registration.activation === 'optional',
  };
  await writeFile(join(packageRoot, 'plugin.manifest.json'), `${JSON.stringify(packageManifest, null, 2)}\n`);
}

// Optional viewers are discovered by the native service from viewers/.
// Bootstrap viewers remain statically imported by the app shell.
await writeFile(outputPath, `${JSON.stringify({ plugins: [] }, null, 2)}\n`);
console.log(`Wrote ${outputPath}`);
