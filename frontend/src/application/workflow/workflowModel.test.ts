import { describe, expect, it } from 'vitest';
import type { WorkflowDefinition } from '../../framework/backend/protocol';
import {
  canvasWorkflow,
  nextCanvasNodeNumber,
  recordCanvasHistory,
  redoCanvasHistory,
  undoCanvasHistory,
  workflowFingerprint,
  workflowLayout,
  workflowToCanvas,
} from './workflowModel';

describe('workflow model', () => {
  it('produces deterministic canvas layout positions', () => {
    expect(workflowLayout(0)).toEqual({ x: 700, y: 650 });
    expect(workflowLayout(3)).toEqual({ x: 700, y: 990 });
  });

  it('finds the next canvas node number from existing ids', () => {
    expect(nextCanvasNodeNumber([{ id: 'operation-2' }, { id: 'operation-8' }] as never)).toBe(9);
    expect(nextCanvasNodeNumber([])).toBe(1);
  });

  it('fingerprints equivalent workflows independently of object key order', () => {
    const first: WorkflowDefinition = {
      schema_version: 1,
      operations: [],
      connections: [],
      workflow_id: 'workflow',
      version: 1,
    };
    const second: WorkflowDefinition = {
      connections: [],
      workflow_id: 'workflow',
      operations: [],
      schema_version: 1,
      version: 1,
    };
    expect(workflowFingerprint(first)).toBe(workflowFingerprint(second));
  });

  it('serializes canvas nodes and edges through the backend capability contract', () => {
    const workflow = canvasWorkflow(
      [{ id: 'operation-1', capabilityId: 'sf:run', x: 10, y: 20 } as never],
      [{ id: 'edge-1', source: 'operation-1', sourcePort: 'out', target: 'operation-2', targetPort: 'in' } as never],
      {
        operations: [{ canonical_id: 'sf:run', parameters: [{ name: 'known', schema: { type: 'string' } }] }],
      } as never,
      3,
    );
    expect(workflow).toMatchObject({ version: 3, operations: [{ operation: 'sf:run', position: { x: 10, y: 20 } }] });
    expect(workflow.connections).toEqual([
      { source_operation: 'operation-1', source_port: 'out', target_operation: 'operation-2', target_port: 'in' },
    ]);
  });

  it('updates arbitrary workflow metadata without duplicating the workflow revision', () => {
    const workflow = canvasWorkflow([], [], { operations: [] } as never, 15, {
      owner: 'web-app',
      purpose: 'metadata update test',
    });
    expect(workflow.metadata).toEqual({ owner: 'web-app', purpose: 'metadata update test' });
    expect(workflow.version).toBe(15);
    expect(workflow.metadata).not.toHaveProperty('version');
  });

  it('hydrates canvas nodes from a workflow using capability metadata', () => {
    const result = workflowToCanvas(
      {
        schema_version: 1,
        workflow_id: 'workflow',
        version: 1,
        operations: [{ id: 'operation-1', operation: 'sf:run', parameters: { known: 'value' } }],
        connections: [],
      },
      { operations: [{ canonical_id: 'sf:run', label: 'Run', definition: 'Execute', parameters: [] }] } as never,
    );
    expect(result.nodes[0]).toMatchObject({ title: 'Run', capabilityId: 'sf:run', x: 700, y: 650 });
  });

  it('keeps undo and redo history for canvas snapshots', () => {
    const first = { nodes: [], edges: [] };
    const second = { nodes: [{ id: 'operation-1' }], edges: [] } as never;
    const initial = { past: [], future: [], current: '' };
    const recorded = recordCanvasHistory(recordCanvasHistory(initial, first), second);
    const undone = undoCanvasHistory(recorded);
    expect(undone?.snapshot).toEqual(first);
    expect(redoCanvasHistory(undone!.history)?.snapshot).toEqual(second);
  });
});
