import { describe, expect, it } from 'vitest';
import { edgeTypeKey, portPosition } from './portGeometry';

describe('port geometry', () => {
  const node = { id: 'operation-1', x: 100, y: 200 } as never;

  it('prefers measured anchor positions', () => {
    expect(portPosition(node, undefined, 'output', 'output', { x: 12, y: 34 }, false)).toEqual({ x: 12, y: 34 });
  });

  it('places fallback output ports on the node edge', () => {
    expect(portPosition(node, undefined, 'output', 'output', undefined, false)).toEqual({ x: 336, y: 294 });
  });

  it('resolves parameter and unknown connector types', () => {
    const capability = {
      parameters: [{ name: 'threshold', schema: { type: 'number' } }],
    } as never;
    expect(edgeTypeKey(capability, 'parameter:threshold', 'input')).toBe('number');
    expect(edgeTypeKey(capability, 'missing', 'output')).toBe('unknown');
  });
});
