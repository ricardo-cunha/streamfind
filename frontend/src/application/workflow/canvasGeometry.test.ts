import { describe, expect, it } from 'vitest';
import { connectedNodePosition, NODE_HEIGHT, NODE_WIDTH } from './canvasGeometry';

describe('canvas geometry', () => {
  it('places a connected node without overlapping existing nodes', () => {
    const source = { id: 'source', x: 0, y: 0 } as never;
    const position = connectedNodePosition(source, [source, { id: 'occupied', x: NODE_WIDTH + 72, y: 0 } as never]);
    expect(position.x).toBe(NODE_WIDTH + 72);
    expect(position.y).not.toBe(0);
  });

  it('uses stable canvas dimensions', () => {
    expect(NODE_WIDTH).toBe(224);
    expect(NODE_HEIGHT).toBe(260);
  });
});
