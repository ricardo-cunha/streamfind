import type { ReactNode } from 'react';
import type { ResultRecord } from '../backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../plugins/pluginTypes';
import { ViewerResolver } from './ViewerResolver';
import { ViewerShell } from './ViewerShell';
import { ViewerErrorBoundary } from './ViewerErrorBoundary';

export type ArtifactViewerHostProps = {
  artifact: ResultRecord;
  sessionId: string;
  viewerId?: string;
  mode?: 'table' | 'default';
  pluginApi?: FrontendPluginApi;
  title?: string;
  variant?: 'json' | 'wide' | 'visualization';
  showHeader?: boolean;
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
  showHeader = true,
  onClose,
  fallback,
}: ArtifactViewerHostProps) {
  const context = {
    sessionId,
    resultId: artifact.result_id,
    semanticType: artifact.contract_id,
    artifact,
    pluginApi,
  };

  return (
    <ViewerShell
      title={title}
      subtitle={`${artifact.representation} · result ${artifact.result_id}`}
      variant={variant}
      showHeader={showHeader}
      onClose={onClose}
    >
      <ViewerErrorBoundary resultId={artifact.result_id}>
        {viewerId || mode === 'table' || artifact.representation === 'table' ? (
          <ViewerResolver viewerId={viewerId} context={context} fallback={fallback} />
        ) : (
          fallback
        )}
      </ViewerErrorBoundary>
    </ViewerShell>
  );
}
