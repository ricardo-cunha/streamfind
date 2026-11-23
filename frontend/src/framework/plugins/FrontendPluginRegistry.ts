import { notifyApp } from '../notifications/notificationBus';
import { artifactContractMatches, isQualifiedArtifactContract, localArtifactContract } from '../artifacts/artifactContracts';
import { StreamFindApiClient } from '../backend/StreamFindApiClient';
import {
  viewerRegistry,
  viewerMatchesArtifact,
  viewerMatchesManifest,
  type ViewerRegistry,
  type ViewerRegistration,
} from '../viewers/viewerTypes';
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
  }

  has(id: string): boolean {
    return this.plugins.has(id);
  }

  unregister(id: string): boolean {
    if (!this.plugins.has(id)) return false;
    for (const viewerId of this.setupViewerIds.get(id) ?? []) this.viewers.unregister(viewerId);
    for (const rendererId of this.setupRendererIds.get(id) ?? []) this.visualizations.unregister(rendererId);
    this.setupViewerIds.delete(id);
    this.setupRendererIds.delete(id);
    this.plugins.delete(id);
    return true;
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

  compatibleArtifactViewers(
    pluginId: string,
    query: { semanticType: string; representation?: string },
  ): ViewerRegistration[] {
    const matches = new Map<string, ViewerRegistration>();
    for (const id of this.viewers.ids()) {
      const viewer = this.viewers.get(id);
      if (!viewer) continue;

      if (viewer.accepts.length === 0) {
        if (viewerMatchesArtifact(viewer, query)) matches.set(viewer.id, viewer);
        continue;
      }

      const contracts = isQualifiedArtifactContract(query.semanticType)
        ? viewer.accepts.filter((contract) => contract === query.semanticType)
        : viewer.accepts.filter((contract) => {
            if (localArtifactContract(contract) !== query.semanticType) return false;
            const declaredByPlugin = [...this.plugins.values()].some(
              (entry) =>
                entry.manifest.plugins?.includes(pluginId) &&
                entry.manifest.artifactContracts?.some((declared) => declared === contract),
            );
            return declaredByPlugin || contract.includes(`/plugins/${pluginId}#`);
          });

      if (contracts.some((semanticType) => viewerMatchesArtifact(viewer, { ...query, semanticType }))) {
        matches.set(viewer.id, viewer);
      }
    }
    return [...matches.values()].sort((left, right) => left.id.localeCompare(right.id));
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
        id: artifact.result_id,
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
    const compatible = viewerMatchesManifest({ accepts: viewer.accepts, representations: viewer.representations }, contracts, representations);
    if ((contracts.length || representations.length) && !compatible) {
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
