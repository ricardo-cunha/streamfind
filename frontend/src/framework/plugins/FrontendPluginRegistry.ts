import { notifyApp } from '../notifications/notificationBus';
import { artifactContractMatches } from '../artifacts/artifactContracts';
import { StreamFindApiClient } from '../backend/StreamFindApiClient';
import { viewerRegistry, type ViewerRegistry, type ViewerRegistration } from '../viewers/viewerTypes';
import { visualizationRegistry, type VisualizationRegistry } from '../visualization/VisualizationRegistry';
import {
  FRONTEND_PLUGIN_API_VERSION,
  type FrontendPlugin,
  type FrontendPluginApi,
  type FrontendThemeTokens,
} from './pluginTypes';

function readThemeTokens(): FrontendThemeTokens {
  if (typeof document === 'undefined') return {};
  const styles = getComputedStyle(document.documentElement);
  return Object.freeze({
    foreground: styles.getPropertyValue('--foreground').trim(),
    mutedForeground: styles.getPropertyValue('--muted-foreground').trim(),
    accent: styles.getPropertyValue('--accent').trim(),
    border: styles.getPropertyValue('--border').trim(),
    card: styles.getPropertyValue('--card').trim(),
  });
}

export class FrontendPluginRegistry {
  private readonly plugins = new Map<string, FrontendPluginManifestEntry>();
  private readonly setupViewerIds = new Map<string, string[]>();
  private readonly setupRendererIds = new Map<string, string[]>();

  constructor(
    private readonly viewers: ViewerRegistry = viewerRegistry,
    private readonly client: StreamFindApiClient = new StreamFindApiClient(),
    private readonly visualizations: VisualizationRegistry = visualizationRegistry,
  ) {}

  register(plugin: FrontendPlugin): void {
    validateManifest(plugin);
    if (this.plugins.has(plugin.manifest.id)) {
      throw new Error(`Frontend plugin already registered: ${plugin.manifest.id}`);
    }

    this.plugins.set(plugin.manifest.id, {
      manifest: plugin.manifest,
    });
    this.setupViewerIds.set(plugin.manifest.id, []);
    this.setupRendererIds.set(plugin.manifest.id, []);
    try {
      plugin.setup(this.createApi(plugin.manifest.id));
    } catch (error) {
      for (const viewerId of this.setupViewerIds.get(plugin.manifest.id) ?? []) this.viewers.unregister(viewerId);
      for (const rendererId of this.setupRendererIds.get(plugin.manifest.id) ?? [])
        this.visualizations.unregister(rendererId);
      this.setupViewerIds.delete(plugin.manifest.id);
      this.setupRendererIds.delete(plugin.manifest.id);
      this.plugins.delete(plugin.manifest.id);
      throw error;
    }
    this.setupViewerIds.delete(plugin.manifest.id);
    this.setupRendererIds.delete(plugin.manifest.id);
  }

  has(id: string): boolean {
    return this.plugins.has(id);
  }

  ids(): string[] {
    return [...this.plugins.keys()].sort();
  }

  providersForArtifact(contract: string): string[] {
    return [...this.plugins.values()]
      .filter((entry) =>
        (entry.manifest.artifactContracts ?? []).some((declared) => artifactContractMatches(declared, contract)),
      )
      .map((entry) => entry.manifest.id)
      .sort();
  }

  api(client: StreamFindApiClient = this.client): FrontendPluginApi {
    return this.createApi('runtime', client);
  }

  private createApi(pluginId: string, client = this.client): FrontendPluginApi {
    return {
      apiVersion: FRONTEND_PLUGIN_API_VERSION,
      client,
      viewerRegistry: this.viewers,
      visualizationRegistry: this.visualizations,
      theme: readThemeTokens(),
      registerViewer: (viewer) => this.registerViewer(pluginId, viewer),
      registerVisualizationRenderer: (rendererId, renderer) =>
        this.registerVisualizationRenderer(pluginId, rendererId, renderer),
      notify: notifyApp,
      artifactSummary: (artifact) => ({
        id: artifact.artifact_id,
        contract: artifact.contract_id,
        representation: artifact.representation,
      }),
    };
  }

  private registerViewer(pluginId: string, viewer: ViewerRegistration): void {
    const plugin = this.plugins.get(pluginId);
    if (!plugin) {
      throw new Error(`Frontend plugin is not active: ${pluginId}`);
    }
    const before = this.viewers.has(viewer.id);
    if (before) throw new Error(`Viewer already registered: ${viewer.id}`);
    const contracts = plugin.manifest.artifactContracts ?? [];
    const representations = plugin.manifest.artifactRepresentations ?? [];
    if (
      (contracts.length || representations.length) &&
      !viewer.accepts.some((accepted) => contracts.some((declared) => sameContract(accepted, declared))) &&
      !(viewer.representations ?? []).some((representation) => representations.includes(representation))
    ) {
      throw new Error(`Viewer contracts are incompatible with frontend plugin: ${viewer.id}`);
    }
    this.viewers.register(viewer);
    this.setupViewerIds.get(pluginId)?.push(viewer.id);
  }

  private registerVisualizationRenderer(
    pluginId: string,
    rendererId: string,
    renderer: Parameters<VisualizationRegistry['register']>[1],
  ): void {
    if (!this.plugins.has(pluginId)) throw new Error(`Frontend plugin is not active: ${pluginId}`);
    this.visualizations.register(rendererId, renderer);
    this.setupRendererIds.get(pluginId)?.push(rendererId);
  }
}

type FrontendPluginManifestEntry = {
  manifest: FrontendPlugin['manifest'];
};

function sameContract(left: string, right: string): boolean {
  const local = (value: string) => value.split('#').at(-1)?.split(':').at(-1) ?? value;
  return left === right || local(left) === local(right);
}

function validateManifest(plugin: FrontendPlugin): void {
  const { manifest } = plugin;
  if (!manifest.id.trim()) throw new Error('Frontend plugin id must not be empty');
  if (!manifest.name.trim()) throw new Error(`Frontend plugin name must not be empty: ${manifest.id}`);
  if (!manifest.version.trim()) throw new Error(`Frontend plugin version must not be empty: ${manifest.id}`);
  if (manifest.apiVersion !== FRONTEND_PLUGIN_API_VERSION) {
    throw new Error(`Unsupported frontend plugin API version: ${manifest.apiVersion}`);
  }
  if (typeof plugin.setup !== 'function') throw new Error(`Frontend plugin setup is not callable: ${manifest.id}`);
}

export const frontendPluginRegistry = new FrontendPluginRegistry();

export function frontendPluginApi(client: StreamFindApiClient): FrontendPluginApi {
  return frontendPluginRegistry.api(client);
}
