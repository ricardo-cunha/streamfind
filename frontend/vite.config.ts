import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig, type Plugin } from 'vitest/config';
import react from '@vitejs/plugin-react';

const frontendRoot = fileURLToPath(new URL('.', import.meta.url));

async function runtimePluginManifest() {
  const pluginDirectories = await readdir(join(frontendRoot, 'src', 'plugins'), { withFileTypes: true });
  const plugins = [];
  for (const directory of pluginDirectories) {
    if (!directory.isDirectory()) continue;
    try {
      const registration = JSON.parse(
        await readFile(join(frontendRoot, 'src', 'plugins', directory.name, 'plugin.manifest.json'), 'utf8'),
      );
      if (registration.activation !== 'optional') continue;
      const { source } = registration;
      const manifest = { ...registration };
      delete manifest.activation;
      delete manifest.source;
      plugins.push({ ...manifest, entry: `/${source}` });
    } catch (error) {
      if ((error as NodeJS.ErrnoException)?.code !== 'ENOENT') throw error;
    }
  }
  return { plugins: plugins.sort((left, right) => left.id.localeCompare(right.id)) };
}

function pluginManifestDevServer(): Plugin {
  return {
    name: 'streamfind-plugin-manifest',
    configureServer(server) {
      server.middlewares.use('/plugins.json', async (_request, response) => {
        try {
          const manifest = await runtimePluginManifest();
          response.statusCode = 200;
          response.setHeader('Content-Type', 'application/json');
          response.end(JSON.stringify(manifest));
        } catch (error) {
          response.statusCode = 500;
          response.end(error instanceof Error ? error.message : String(error));
        }
      });
    },
  };
}

export default defineConfig({
  plugins: [react(), pluginManifestDevServer()],
  server: { host: '127.0.0.1', port: 5173, strictPort: true },
  build: {
    manifest: true,
    // Plotly is an intentionally lazy vendor chunk; keep its size warning out of the application build output.
    chunkSizeWarningLimit: 5000,
    rollupOptions: {
      input: {
        application: 'index.html',
      },
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
    restoreMocks: true,
    setupFiles: ['./src/test/setup.ts'],
  },
});
