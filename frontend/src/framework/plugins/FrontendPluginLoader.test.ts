import { afterEach, describe, expect, it, vi } from 'vitest';
import { FrontendPluginLoader, type RuntimeFrontendPluginManifest } from './FrontendPluginLoader';
import { FrontendPluginRegistry } from './FrontendPluginRegistry';
import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from './pluginTypes';
import { ViewerRegistry } from '../viewers/viewerTypes';

const manifest: RuntimeFrontendPluginManifest = {
  schemaVersion: 1,
  kind: 'viewer',
  id: 'runtime.test',
  name: 'Runtime test plugin',
  version: '1.0.0',
  apiVersion: FRONTEND_PLUGIN_API_VERSION,
  entry: '/plugins/runtime-test.js',
};

function dispatchStylesheetLoad(): void {
  for (const link of document.querySelectorAll<HTMLLinkElement>('link[rel="stylesheet"]')) {
    link.dispatchEvent(new Event('load'));
  }
}

const plugin: FrontendPlugin = {
  manifest,
  setup(api) {
    api.registerViewer({
      id: 'runtime.test.viewer',
      label: 'Runtime test viewer',
      accepts: ['sf:table'],
      component: () => null,
    });
  },
};

describe('frontend plugin loader', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  it('loads a same-origin plugin and activates its registration', async () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    const importer = vi.fn().mockResolvedValue({ default: plugin });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));

    await expect(loader.load(manifest)).resolves.toBe(true);
    expect(importer).toHaveBeenCalledWith('/plugins/runtime-test.js');
    expect(viewers.resolve('sf:table')?.id).toBe('runtime.test.viewer');
    await expect(loader.load(manifest)).resolves.toBe(true);
    expect(importer).toHaveBeenCalledTimes(1);
  });

  it('replaces an already loaded package when its version or digest changes', async () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    const importer = vi.fn().mockResolvedValue({ default: plugin });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));

    await expect(loader.load({ ...manifest, packageDigest: 'digest-a' })).resolves.toBe(true);
    await expect(loader.load({ ...manifest, version: '2.0.0', packageDigest: 'digest-b' })).resolves.toBe(true);

    expect(importer).toHaveBeenCalledTimes(2);
    expect(registry.ids()).toEqual(['runtime.test']);
    expect(viewers.ids()).toEqual(['runtime.test.viewer']);
  });

  it('loads packaged plugin stylesheets before importing the plugin', async () => {
    const registry = new FrontendPluginRegistry(new ViewerRegistry());
    const importer = vi.fn().mockResolvedValue({ default: plugin });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));
    const result = loader.load({ ...manifest, css: ['/assets/mass-spec.css'] });
    dispatchStylesheetLoad();

    await expect(result).resolves.toBe(true);
    expect(document.querySelector<HTMLLinkElement>('link[data-streamfind-plugin="runtime.test"]')?.href).toBe(
      `${window.location.origin}/assets/mass-spec.css`,
    );
    expect(importer).toHaveBeenCalledWith('/plugins/runtime-test.js');
  });

  it('rejects a cross-origin entry without importing it', async () => {
    const importer = vi.fn();
    const loader = new FrontendPluginLoader(
      new FrontendPluginRegistry(new ViewerRegistry()),
      importer,
      new Set([window.location.origin]),
    );

    await expect(loader.load({ ...manifest, entry: 'https://plugins.example.test/plugin.js' })).resolves.toBe(false);
    expect(importer).not.toHaveBeenCalled();
  });

  it('rejects unsupported viewer manifest schema and kind', async () => {
    const importer = vi.fn();
    const loader = new FrontendPluginLoader(
      new FrontendPluginRegistry(new ViewerRegistry()),
      importer,
      new Set([window.location.origin]),
    );

    await expect(loader.load({ ...manifest, schemaVersion: 2 as 1 })).resolves.toBe(false);
    await expect(loader.load({ ...manifest, kind: 'native' as 'viewer' })).resolves.toBe(false);
    expect(importer).not.toHaveBeenCalled();
  });

  it('does not execute a viewer that requests an unavailable sandbox policy', async () => {
    const importer = vi.fn();
    const loader = new FrontendPluginLoader(
      new FrontendPluginRegistry(new ViewerRegistry()),
      importer,
      new Set([window.location.origin]),
    );

    await expect(loader.load({ ...manifest, trust: 'sandboxed' })).resolves.toBe(false);
    expect(importer).not.toHaveBeenCalled();
  });

  it('contains module and setup failures', async () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    const importer = vi.fn().mockResolvedValue({
      default: {
        ...plugin,
        setup: () => {
          throw new Error('broken setup');
        },
      },
    });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));

    await expect(loader.load(manifest)).resolves.toBe(false);
    expect(registry.has(manifest.id)).toBe(false);
    expect(viewers.has('runtime.test.viewer')).toBe(false);
  });

  it('discovers manifests from the packaged same-origin manifest and keeps bootstrap optional', async () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    const importer = vi.fn().mockResolvedValue({ default: plugin });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: true,
        status: 200,
        json: async () => ({ plugins: [manifest] }),
      }),
    );

    await expect(loader.loadManifestUrl()).resolves.toBe(1);
    expect(viewers.resolve('sf:table')?.id).toBe('runtime.test.viewer');
  });

  it('ignores a missing packaged manifest without affecting core startup', async () => {
    const loader = new FrontendPluginLoader(new FrontendPluginRegistry(new ViewerRegistry()), vi.fn());
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: false, status: 404 }));

    await expect(loader.loadManifestUrl()).resolves.toBe(0);
  });

  it('skips malformed and duplicate runtime entries without stopping valid plugins', async () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);
    const importer = vi.fn().mockResolvedValue({ default: plugin });
    const loader = new FrontendPluginLoader(registry, importer, new Set([window.location.origin]));
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: true,
        status: 200,
        json: async () => ({
          plugins: [{ id: 'broken' }, manifest, { ...manifest, entry: '/plugins/runtime-test-duplicate.js' }],
        }),
      }),
    );

    await expect(loader.loadManifestUrl()).resolves.toBe(1);
    expect(importer).toHaveBeenCalledTimes(1);
    expect(registry.has(manifest.id)).toBe(true);
  });
});
