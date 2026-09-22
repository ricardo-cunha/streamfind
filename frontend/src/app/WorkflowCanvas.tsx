import { StreamFindApiClient, type ProjectSession } from '../backend/StreamFindApiClient';
import type { ServiceCapabilities } from '../backend/protocol';
import CanvasShell from './CanvasShell';

export default function WorkflowCanvas({
  project,
  capabilities,
  client,
  onProjectHub,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  client: StreamFindApiClient;
  onProjectHub: () => void;
}) {
  return (
    <CanvasShell
      project={project}
      capabilities={capabilities}
      surface="workflow"
      client={client}
      onProjectHub={onProjectHub}
    />
  );
}
