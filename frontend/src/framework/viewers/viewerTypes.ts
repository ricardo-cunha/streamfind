import type { ReactNode } from 'react';
import { artifactContractMatches } from '../artifacts/artifactContracts';
import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../plugins/pluginTypes';

export type ViewerContext = {
  sessionId: string;
  nodeId?: string;
  portId?: string;
  artifactId: string;
  semanticType: string;
  artifact?: ArtifactRecord;
  pluginApi?: FrontendPluginApi;
};

export type ViewerComponentProps = {
  context: ViewerContext;
};

export type ViewerComponent = (props: ViewerComponentProps) => ReactNode;

export type ViewerRegistration = {
  id: string;
  label: string;
  accepts: string[];
  representations?: string[];
  component: ViewerComponent;
};

export type ViewerQuery = {
  semanticType: string;
  representation?: string;
};

function acceptsContract(accepted: string, actual: string): boolean {
  return artifactContractMatches(accepted, actual);
}

export class ViewerRegistry {
  private readonly viewers = new Map<string, ViewerRegistration>();

  register(viewer: ViewerRegistration): void {
    if (!viewer.id.trim()) throw new Error('Viewer id must not be empty');
    if (!viewer.accepts.length && !viewer.representations?.length)
      throw new Error(`Viewer must accept a semantic contract or representation: ${viewer.id}`);
    if (this.viewers.has(viewer.id)) throw new Error(`Viewer already registered: ${viewer.id}`);
    this.viewers.set(viewer.id, viewer);
  }

  resolve(query: string | ViewerQuery): ViewerRegistration | undefined {
    return this.compatible(query)[0];
  }

  compatible(query: string | ViewerQuery): ViewerRegistration[] {
    const normalized = typeof query === 'string' ? { semanticType: query } : query;
    return [...this.viewers.values()]
      .sort((left, right) => left.id.localeCompare(right.id))
      .filter(
        (viewer) =>
          viewer.accepts.some((accepted) => acceptsContract(accepted, normalized.semanticType)) ||
          (normalized.representation !== undefined &&
            viewer.representations?.includes(normalized.representation) === true),
      );
  }

  get(id: string): ViewerRegistration | undefined {
    return this.viewers.get(id);
  }

  ids(): string[] {
    return [...this.viewers.keys()].sort();
  }

  has(id: string): boolean {
    return this.viewers.has(id);
  }

  unregister(id: string): void {
    this.viewers.delete(id);
  }
}

export const viewerRegistry = new ViewerRegistry();

export function compatibleArtifactViewers(query: ViewerQuery): ViewerRegistration[] {
  return viewerRegistry.compatible(query);
}
