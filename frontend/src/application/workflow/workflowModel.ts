import type {
  ServiceCapabilities,
  WorkflowConnectionDefinition,
  WorkflowDefinition,
  WorkflowOperationDefinition,
} from '../../framework/backend/protocol';
import { wireParameters } from './parameterModel';
import { uiParameters } from './parameterModel';

export type CanvasNodeKind = 'project' | 'operation' | 'method' | 'extract' | 'plot';
export type CanvasNode = {
  id: string;
  kind: CanvasNodeKind;
  title: string;
  description: string;
  x: number;
  y: number;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
  parameters?: Record<string, unknown>;
  executionState?: 'idle' | 'running' | 'completed' | 'failed';
  executionMessage?: string;
};
export type CanvasEdge = {
  id: string;
  source: string;
  sourcePort: string;
  target: string;
  targetPort: string;
};
export type Point = { x: number; y: number };
export type Interaction = {
  type: 'node' | 'pan';
  id?: string;
  offsetX?: number;
  offsetY?: number;
  originX?: number;
  originY?: number;
};
export type ConnectionDraft = { source: string; sourcePort: string; point: Point };
export type CanvasLogLine = { id: number; timestamp: string; message: string; level: 'info' | 'success' | 'error' };
export type CanvasHistorySnapshot = { nodes: CanvasNode[]; edges: CanvasEdge[] };
export type CanvasHistoryState = {
  past: CanvasHistorySnapshot[];
  future: CanvasHistorySnapshot[];
  current: string;
};

export function canvasSnapshotFingerprint(snapshot: CanvasHistorySnapshot): string {
  return JSON.stringify(snapshot);
}

export function canvasSnapshotFromFingerprint(fingerprint: string): CanvasHistorySnapshot {
  return JSON.parse(fingerprint) as CanvasHistorySnapshot;
}

export function recordCanvasHistory(state: CanvasHistoryState, snapshot: CanvasHistorySnapshot): CanvasHistoryState {
  const fingerprint = canvasSnapshotFingerprint(snapshot);
  if (!state.current || state.current === fingerprint) return { ...state, current: fingerprint };
  return {
    past: [...state.past.slice(-49), canvasSnapshotFromFingerprint(state.current)],
    future: [],
    current: fingerprint,
  };
}

export function undoCanvasHistory(
  state: CanvasHistoryState,
): { history: CanvasHistoryState; snapshot: CanvasHistorySnapshot } | null {
  if (!state.past.length) return null;
  const snapshot = state.past.at(-1) as CanvasHistorySnapshot;
  return {
    history: {
      past: state.past.slice(0, -1),
      future: [...state.future, canvasSnapshotFromFingerprint(state.current)],
      current: canvasSnapshotFingerprint(snapshot),
    },
    snapshot,
  };
}

export function redoCanvasHistory(
  state: CanvasHistoryState,
): { history: CanvasHistoryState; snapshot: CanvasHistorySnapshot } | null {
  if (!state.future.length) return null;
  const snapshot = state.future.at(-1) as CanvasHistorySnapshot;
  return {
    history: {
      past: [...state.past, canvasSnapshotFromFingerprint(state.current)],
      future: state.future.slice(0, -1),
      current: canvasSnapshotFingerprint(snapshot),
    },
    snapshot,
  };
}

export function canvasWorkflow(
  nodes: CanvasNode[],
  edges: CanvasEdge[],
  capabilities: ServiceCapabilities,
  revision: number,
  metadata: WorkflowDefinition['metadata'] = {},
): WorkflowDefinition {
  const operations: WorkflowOperationDefinition[] = nodes
    .filter((node) => node.capabilityId)
    .map((node) => ({
      id: node.id,
      operation: node.capabilityId as string,
      inputs: {},
      parameters: wireParameters(
        capabilities.operations.find((capability) => capability.canonical_id === node.capabilityId),
        node.parameters || {},
      ),
      position: { x: node.x, y: node.y },
    }));
  const connections: WorkflowConnectionDefinition[] = edges.map((edge) => ({
    source_operation: edge.source,
    source_port: edge.sourcePort,
    target_operation: edge.target,
    target_port: edge.targetPort,
  }));
  return {
    schema_version: 1,
    workflow_id: 'workflow',
    name: 'Workflow',
    version: revision,
    metadata,
    operations,
    connections,
  };
}

export function workflowToCanvas(
  workflow: WorkflowDefinition,
  capabilities: ServiceCapabilities,
): { nodes: CanvasNode[]; edges: CanvasEdge[] } {
  const nodes = workflow.operations.map((operation, index) => {
    const capability = capabilities.operations.find((item) => item.canonical_id === operation.operation);
    const position =
      operation.position && Number.isFinite(operation.position.x) && Number.isFinite(operation.position.y)
        ? operation.position
        : workflowLayout(index);
    return {
      id: operation.id,
      kind: 'operation' as const,
      title: capability?.label || operation.operation,
      description: capability?.definition || '',
      x: position.x,
      y: position.y,
      outputsToCanvas: true,
      capabilityId: operation.operation,
      projectEntry: capability?.project_entry === true,
      parameters: uiParameters(capability, operation.parameters),
      executionState: 'idle' as const,
    };
  });
  const edges = workflow.connections.map((connection) => ({
    id: `${connection.source_operation}-${connection.source_port}-${connection.target_operation}-${connection.target_port}`,
    source: connection.source_operation,
    sourcePort: connection.source_port,
    target: connection.target_operation,
    targetPort: connection.target_port,
  }));
  return { nodes, edges };
}

export function workflowLayout(index: number): Point {
  return { x: 700 + (index % 3) * 420, y: 650 + Math.floor(index / 3) * 340 };
}

function canonicalWorkflowValue(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalWorkflowValue);
  if (value && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, item]) => [key, canonicalWorkflowValue(item)]),
    );
  }
  return value;
}

export function workflowFingerprint(workflow: WorkflowDefinition): string {
  return JSON.stringify(
    canonicalWorkflowValue({
      schema_version: workflow.schema_version,
      workflow_id: workflow.workflow_id || 'workflow',
      operations: workflow.operations,
      connections: workflow.connections,
    }),
  );
}

export function nextCanvasNodeNumber(nodes: CanvasNode[]): number {
  return (
    Math.max(
      0,
      ...nodes.map((node) => {
        const match = /-(\d+)$/.exec(node.id);
        return match ? Number(match[1]) : 0;
      }),
    ) + 1
  );
}
