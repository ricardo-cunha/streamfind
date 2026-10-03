import type { ArtifactRecord, StreamFindApiClient } from '../backend/StreamFindApiClient';
import type { AppNotification, NotificationKind } from '../notifications/notificationBus';
import type { ViewerRegistration, ViewerRegistry } from '../viewers/viewerTypes';
import type { VisualizationRenderer, VisualizationRegistry } from '../visualization/VisualizationRegistry';

export const FRONTEND_PLUGIN_API_VERSION = '1.0' as const;

export type FrontendPluginManifest = {
  id: string;
  name: string;
  version: string;
  apiVersion: typeof FRONTEND_PLUGIN_API_VERSION;
  artifactContracts?: string[];
  artifactRepresentations?: string[];
  visualizationTypes?: string[];
  domains?: string[];
  capabilities?: string[];
};

export type FrontendThemeTokens = Readonly<Record<string, string>>;

export type FrontendPluginApi = {
  readonly apiVersion: typeof FRONTEND_PLUGIN_API_VERSION;
  readonly client: StreamFindApiClient;
  readonly viewerRegistry: ViewerRegistry;
  readonly visualizationRegistry: VisualizationRegistry;
  readonly theme: FrontendThemeTokens;
  registerViewer(viewer: ViewerRegistration): void;
  registerVisualizationRenderer(rendererId: string, renderer: VisualizationRenderer): void;
  notify(input: { kind: NotificationKind; message: string }): AppNotification;
  artifactSummary(artifact: ArtifactRecord): { id: string; contract: string; representation: string };
};

export type FrontendPlugin = {
  manifest: FrontendPluginManifest;
  setup(api: FrontendPluginApi): void;
};
