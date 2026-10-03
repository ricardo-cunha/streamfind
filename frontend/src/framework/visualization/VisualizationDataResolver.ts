import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import { parseVisualizationSpec, type VisualizationSpec } from './visualizationTypes';

export function visualizationSpecFromArtifact(artifact: ArtifactRecord): VisualizationSpec {
  const contract = artifact.contract_id.split('#').at(-1)?.split(':').at(-1) ?? artifact.contract_id;
  if (contract !== 'visualizationSpecResult') {
    throw new Error(`Artifact is not a visualization specification: ${artifact.contract_id}`);
  }
  if (artifact.representation !== 'json') {
    throw new Error(`Visualization artifact must use JSON representation: ${artifact.representation}`);
  }
  return parseVisualizationSpec(artifact.payload);
}
