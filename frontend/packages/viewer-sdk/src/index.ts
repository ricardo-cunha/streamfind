import type { ReactNode } from 'react';

export const VIEWER_SDK_API_VERSION = '1.0' as const;
export const FRONTEND_PLUGIN_API_VERSION = '1.0' as const;

export type ArtifactSummary = {
  id: string;
  contract: string;
  representation: string;
};

export type ViewerContext = {
  sessionId: string;
  nodeId?: string;
  portId?: string;
  resultId: string;
  semanticType: string;
  artifact?: {
    result_id: string;
    contract_id: string;
    representation: string;
    payload: unknown;
  };
};

export type ViewerComponent = (props: { context: ViewerContext }) => ReactNode;

export type ViewerRegistration = {
  id: string;
  label: string;
  accepts: string[];
  representations?: string[];
  component: ViewerComponent;
};

export type FrontendPluginManifest = {
  schemaVersion: 1;
  kind: 'viewer';
  id: string;
  name: string;
  version: string;
  apiVersion: typeof FRONTEND_PLUGIN_API_VERSION;
  artifactContracts?: string[];
  artifactRepresentations?: string[];
  plugins?: string[];
  capabilities?: string[];
};

export type FrontendPluginApi = {
  readonly apiVersion: typeof FRONTEND_PLUGIN_API_VERSION;
  readonly theme: Readonly<Record<string, string>>;
  registerViewer(viewer: ViewerRegistration): void;
  artifactSummary(artifact: ViewerContext['artifact']): ArtifactSummary;
};

export type FrontendPlugin = {
  manifest: FrontendPluginManifest;
  setup(api: FrontendPluginApi): void;
};

export function artifactContractMatches(left: string, right: string): boolean {
  if (!left || !right) return false;
  return left === right;
}
