import { StreamFindApiClient, type ProjectSession } from '../framework/backend/StreamFindApiClient';
import type { ServiceCapabilities } from '../framework/backend/protocol';
import CanvasShell from './CanvasShell';

export default function WorkflowCanvas({
  project,
  capabilities,
  client,
  onProjectHub,
  onOpenOntologyWiki,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  client: StreamFindApiClient;
  onProjectHub: () => void;
  onOpenOntologyWiki?: (term?: string) => void;
}) {
  return (
    <CanvasShell
      project={project}
      capabilities={capabilities}
      surface="workflow"
      client={client}
      onProjectHub={onProjectHub}
      onOpenOntologyWiki={onOpenOntologyWiki}
    />
  );
}
