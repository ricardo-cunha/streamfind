import type { BackendCapability } from '../../framework/backend/protocol';
import type { CanvasNode, Point } from './workflowModel';
import { NODE_WIDTH } from './canvasGeometry';
import { nodePorts, parameterTypeKey, visualPortTypeKey } from './portModel';

export function portPosition(
  node: CanvasNode,
  capability: BackendCapability | undefined,
  portId: string,
  direction: 'input' | 'output',
  measured: Point | undefined,
  expanded: boolean,
): Point {
  if (measured) return measured;
  const ports = nodePorts(capability)[direction === 'input' ? 'inputs' : 'outputs'];
  if (direction === 'input' && portId.startsWith('parameter:')) {
    const parameterIndex = (capability?.parameters || [])
      .filter((parameter) => parameter.name !== 'database_path')
      .findIndex((parameter) => `parameter:${parameter.name}` === portId);
    const inputCount = ports.length;
    const outputCount = Math.max(1, nodePorts(capability).outputs.length);
    const parametersTop = 58 + (inputCount ? 25 + inputCount * 31 : 0) + 25 + outputCount * 31 + 54;
    return {
      x: node.x - 12,
      y: node.y + parametersTop + (expanded ? Math.max(0, parameterIndex) * 38 + 8 : 15),
    };
  }
  const index = Math.max(
    0,
    ports.findIndex((port) => port.id === portId),
  );
  const top =
    direction === 'input'
      ? 94 + index * 31
      : (ports.length && nodePorts(capability).inputs.length ? 120 + nodePorts(capability).inputs.length * 31 : 94) +
        index * 31;
  return { x: node.x + (direction === 'input' ? -12 : NODE_WIDTH + 12), y: node.y + top };
}

export function edgeTypeKey(
  capability: BackendCapability | undefined,
  portId: string,
  direction: 'input' | 'output',
): string {
  if (direction === 'input' && portId.startsWith('parameter:')) {
    const parameter = capability?.parameters.find((candidate) => `parameter:${candidate.name}` === portId);
    return parameter ? parameterTypeKey(parameter) : 'unknown';
  }
  const port = nodePorts(capability)[direction === 'input' ? 'inputs' : 'outputs'].find(
    (candidate) => candidate.id === portId,
  );
  return port ? visualPortTypeKey(port) : 'unknown';
}
