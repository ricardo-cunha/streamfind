import type { ReactNode } from 'react';
import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../plugins/pluginTypes';
import { ViewerResolver } from './ViewerResolver';
import { ViewerShell } from './ViewerShell';
import { ViewerErrorBoundary } from './ViewerErrorBoundary';

export type ArtifactViewerHostProps = {
  artifact: ArtifactRecord;
  sessionId: string;
  viewerId?: string;
  mode?: 'table' | 'default';
  pluginApi?: FrontendPluginApi;
  title?: string;
  variant?: 'json' | 'wide' | 'visualization';
  onClose: () => void;
  fallback: ReactNode;
};

export function ArtifactViewerHost({
  artifact,
  sessionId,
  viewerId,
  mode = 'default',
  pluginApi,
  title = artifact.contract_id,
  variant = viewerId || artifact.representation === 'table' || mode === 'table' ? 'wide' : 'json',
  onClose,
  fallback,
}: ArtifactViewerHostProps) {
  const context = {
    sessionId,
    artifactId: artifact.artifact_id,
    semanticType: artifact.contract_id,
    artifact,
    pluginApi,
  };

  return (
    <ViewerShell
      title={title}
      subtitle={`${artifact.representation} · ${artifact.artifact_id}`}
      variant={variant}
      onClose={onClose}
    >
      <ViewerErrorBoundary artifactId={artifact.artifact_id}>
        {viewerId || mode === 'table' || artifact.representation === 'table' ? (
          <ViewerResolver viewerId={viewerId} context={context} fallback={fallback} />
        ) : (
          fallback
        )}
      </ViewerErrorBoundary>
    </ViewerShell>
  );
}
