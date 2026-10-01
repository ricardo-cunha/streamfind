import type { ReactNode } from 'react';
import type { ArtifactRecord, StreamFindApiClient } from '../backend/StreamFindApiClient';

export type ViewerContext = {
  sessionId: string;
  nodeId?: string;
  portId?: string;
  artifactId: string;
  semanticType: string;
  artifact?: ArtifactRecord;
  api?: StreamFindApiClient;
};

export type ViewerComponentProps = {
  context: ViewerContext;
};

export type ViewerComponent = (props: ViewerComponentProps) => ReactNode;

export type ViewerRegistration = {
  id: string;
  label: string;
  accepts: string[];
  component: ViewerComponent;
};

function localContract(value: string): string {
  return value.split('#').at(-1)?.split(':').at(-1) ?? value;
}

function acceptsContract(accepted: string, actual: string): boolean {
  return accepted === actual || localContract(accepted) === localContract(actual);
}

export class ViewerRegistry {
  private readonly viewers = new Map<string, ViewerRegistration>();

  register(viewer: ViewerRegistration): void {
    if (!viewer.id.trim()) throw new Error('Viewer id must not be empty');
    if (!viewer.accepts.length) throw new Error(`Viewer must accept a semantic contract: ${viewer.id}`);
    if (this.viewers.has(viewer.id)) throw new Error(`Viewer already registered: ${viewer.id}`);
    this.viewers.set(viewer.id, viewer);
  }

  resolve(semanticType: string): ViewerRegistration | undefined {
    return [...this.viewers.values()]
      .sort((left, right) => left.id.localeCompare(right.id))
      .find((viewer) => viewer.accepts.some((accepted) => acceptsContract(accepted, semanticType)));
  }

  ids(): string[] {
    return [...this.viewers.keys()].sort();
  }

  has(id: string): boolean {
    return this.viewers.has(id);
  }
}

export const viewerRegistry = new ViewerRegistry();
