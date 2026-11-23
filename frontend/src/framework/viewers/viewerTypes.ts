import type { ReactNode } from 'react';
import { artifactContractMatches } from '../artifacts/artifactContracts';
import type { ResultRecord } from '../backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../plugins/pluginTypes';

export type ViewerContext = {
  sessionId: string;
  nodeId?: string;
  portId?: string;
  resultId: string;
  semanticType: string;
  artifact?: ResultRecord;
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

export function viewerMatchesArtifact(
  viewer: Pick<ViewerRegistration, 'accepts' | 'representations'>,
  query: ViewerQuery,
): boolean {
  const contractMatch = viewer.accepts.length === 0 || viewer.accepts.some((accepted) => acceptsContract(accepted, query.semanticType));
  const representationMatch =
    viewer.representations === undefined ||
    (query.representation !== undefined && viewer.representations.includes(query.representation));
  return contractMatch && representationMatch;
}

export function viewerMatchesManifest(
  viewer: Pick<ViewerRegistration, 'accepts' | 'representations'>,
  contracts: readonly string[],
  representations: readonly string[],
): boolean {
  const contractsMatch =
    viewer.accepts.length === 0 ||
    (contracts.length > 0 && viewer.accepts.every((accepted) => contracts.some((declared) => artifactContractMatches(accepted, declared))));
  const representationsMatch =
    viewer.representations === undefined ||
    (representations.length > 0 && viewer.representations.every((representation) => representations.includes(representation)));
  return contractsMatch && representationsMatch;
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
      .filter((viewer) => viewerMatchesArtifact(viewer, normalized));
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
