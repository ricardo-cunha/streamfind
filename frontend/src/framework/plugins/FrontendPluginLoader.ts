import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin, type FrontendPluginManifest } from './pluginTypes';
import { FrontendPluginRegistry } from './FrontendPluginRegistry';
import { notifyApp } from '../notifications/notificationBus';

export type RuntimeFrontendPluginManifest = FrontendPluginManifest & {
  entry: string;
};

type PluginModule = { default?: FrontendPlugin; plugin?: FrontendPlugin };
type Importer = (entry: string) => Promise<PluginModule>;

export class FrontendPluginLoader {
  private readonly loaded = new Set<string>();

  constructor(
    private readonly registry: FrontendPluginRegistry,
    private readonly importer: Importer = (entry) => import(/* @vite-ignore */ entry),
    private readonly allowedOrigins: ReadonlySet<string> = new Set(
      typeof window === 'undefined' ? [] : [window.location.origin],
    ),
  ) {}

  async loadManifestUrl(manifestUrl = '/plugins.json'): Promise<number> {
    try {
      const url = new URL(manifestUrl, window.location.href);
      if (url.origin !== window.location.origin) throw new Error(`Manifest origin is not allowed: ${url.origin}`);
      const response = await fetch(url);
      if (response.status === 404) return 0;
      if (!response.ok) throw new Error(`Manifest request failed (${response.status})`);
      const payload = (await response.json()) as
        RuntimeFrontendPluginManifest[] | { plugins?: RuntimeFrontendPluginManifest[] };
      const manifests = Array.isArray(payload) ? payload : (payload.plugins ?? []);
      if (!Array.isArray(manifests)) throw new Error('Frontend plugin manifest must contain a plugins array');
      const ids = new Set<string>();
      let loaded = 0;
      for (const manifest of manifests) {
        if (ids.has(manifest.id)) {
          notifyApp({ kind: 'warning', message: `Duplicate frontend plugin skipped: ${manifest.id}` });
          continue;
        }
        ids.add(manifest.id);
        if (await this.load(manifest)) loaded += 1;
      }
      return loaded;
    } catch (error) {
      notifyApp({
        kind: 'warning',
        message: `Frontend plugin manifest discovery failed (${error instanceof Error ? error.message : 'invalid response'})`,
      });
      return 0;
    }
  }

  async load(manifest: RuntimeFrontendPluginManifest): Promise<boolean> {
    try {
      validateManifest(manifest, this.allowedOrigins);
      if (this.loaded.has(manifest.id)) return true;
      const module = await this.importer(manifest.entry);
      const plugin = module.default ?? module.plugin;
      if (!plugin) throw new Error(`Frontend plugin module has no plugin export: ${manifest.id}`);
      if (plugin.manifest.id !== manifest.id) {
        throw new Error(`Frontend plugin ID does not match its manifest: ${manifest.id}`);
      }
      this.registry.register(plugin);
      this.loaded.add(manifest.id);
      return true;
    } catch (error) {
      notifyApp({
        kind: 'warning',
        message: `Frontend plugin was not loaded: ${manifest.id} (${error instanceof Error ? error.message : 'unknown error'})`,
      });
      return false;
    }
  }
}

function validateManifest(manifest: RuntimeFrontendPluginManifest, allowedOrigins: ReadonlySet<string>): void {
  if (!manifest || typeof manifest !== 'object') throw new Error('Frontend plugin manifest entry must be an object');
  if (typeof manifest.id !== 'string' || !manifest.id.trim()) throw new Error('Frontend plugin id must not be empty');
  if (typeof manifest.name !== 'string' || !manifest.name.trim())
    throw new Error(`Frontend plugin name must not be empty: ${manifest.id}`);
  if (typeof manifest.version !== 'string' || !manifest.version.trim())
    throw new Error(`Frontend plugin version must not be empty: ${manifest.id}`);
  if (manifest.apiVersion !== FRONTEND_PLUGIN_API_VERSION) {
    throw new Error(`Unsupported frontend plugin API version: ${manifest.apiVersion}`);
  }
  if (typeof manifest.entry !== 'string' || !manifest.entry.trim())
    throw new Error(`Frontend plugin entry must not be empty: ${manifest.id}`);
  for (const field of ['domains', 'artifactContracts', 'artifactRepresentations', 'visualizationTypes']) {
    const value = manifest[field as keyof RuntimeFrontendPluginManifest];
    if (value !== undefined && (!Array.isArray(value) || value.some((item) => typeof item !== 'string'))) {
      throw new Error(`Frontend plugin manifest field must be a string array: ${manifest.id}.${field}`);
    }
  }

  const url = new URL(manifest.entry, typeof window === 'undefined' ? 'http://localhost/' : window.location.href);
  if (url.protocol !== 'http:' && url.protocol !== 'https:') {
    throw new Error(`Unsupported frontend plugin entry protocol: ${url.protocol}`);
  }
  if (allowedOrigins.size && !allowedOrigins.has(url.origin)) {
    throw new Error(`Frontend plugin entry origin is not allowed: ${url.origin}`);
  }
}
