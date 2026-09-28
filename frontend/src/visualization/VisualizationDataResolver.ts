import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import { parseVisualizationSpec, type VisualizationSpec } from './visualizationTypes';

export function visualizationSpecFromArtifact(artifact: ArtifactRecord): VisualizationSpec {
  if (artifact.contract_id !== 'visualizationSpecResult') {
    throw new Error(`Artifact is not a visualization specification: ${artifact.contract_id}`);
  }
  if (artifact.representation !== 'json') {
    throw new Error(`Visualization artifact must use JSON representation: ${artifact.representation}`);
  }
  return parseVisualizationSpec(artifact.payload);
}
