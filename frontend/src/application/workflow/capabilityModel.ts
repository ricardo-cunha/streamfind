import type { BackendCapability, ServiceCapabilities } from '../../framework/backend/protocol';
import type { CanvasNodeKind } from './workflowModel';

export type NodeTemplate = {
  id: string;
  kind: Exclude<CanvasNodeKind, 'project'>;
  title: string;
  description: string;
  icon: string;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
  domain?: string;
  module?: string;
};

export type DocumentationFocus = { section: 'inputs' | 'outputs' | 'parameters'; key: string };

export function operationDeckDensity(title: string, metadata: string): 'regular' | 'compact' | 'dense' {
  const totalLength = title.length + metadata.length;
  if (title.length > 36 || totalLength > 78) return 'dense';
  if (title.length > 25 || totalLength > 58) return 'compact';
  return 'regular';
}

export function capabilityTemplates(capabilities: ServiceCapabilities): NodeTemplate[] {
  return capabilities.operations
    .filter((capability) => capability.kind === 'operation')
    .map((capability: BackendCapability) => ({
      id: capability.canonical_id,
      kind: 'operation' as const,
      title: capability.label,
      description: capability.definition,
      icon: capability.kind === 'operation' ? 'fa-solid fa-bolt' : 'fa-solid fa-gears',
      outputsToCanvas: capability.kind === 'operation',
      capabilityId: capability.canonical_id,
      projectEntry: capability.project_entry === true,
      domain: capability.domain,
      module: capability.module_id,
    }));
}
