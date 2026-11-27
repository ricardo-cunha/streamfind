/**
 * Stable viewer SDK surface.  Host internals may evolve behind these types;
 * independently built viewers should import contracts from this entry point.
 */
export { artifactContractMatches, isQualifiedArtifactContract, localArtifactContract } from '../artifacts/artifactContracts';
export type {
  ViewerComponent,
  ViewerComponentProps,
  ViewerContext,
  ViewerQuery,
  ViewerRegistration,
} from '../viewers/viewerTypes';
export type {
  FrontendPlugin,
  FrontendPluginApi,
  FrontendPluginManifest,
  FrontendThemeTokens,
} from '../plugins/pluginTypes';
export { FRONTEND_PLUGIN_API_VERSION, FRONTEND_PLUGIN_MANIFEST_KIND, FRONTEND_PLUGIN_MANIFEST_SCHEMA_VERSION } from '../plugins/pluginTypes';

export const VIEWER_SDK_API_VERSION = '1.0' as const;
