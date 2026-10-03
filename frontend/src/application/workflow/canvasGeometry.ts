import type { CanvasNode, Point } from './workflowModel';

export const NODE_WIDTH = 224;
export const NODE_HEIGHT = 260;
export const CONNECTED_NODE_GAP = 72;
export const CANVAS_WORLD_WIDTH = 6000;
export const CANVAS_WORLD_HEIGHT = 4000;
export const GRID_SIZE = 22;

export function connectedNodePosition(source: CanvasNode, nodes: CanvasNode[]): Point {
  const columnStep = NODE_WIDTH + CONNECTED_NODE_GAP;
  const rowStep = NODE_HEIGHT + CONNECTED_NODE_GAP;
  for (let column = 1; column <= 20; column += 1) {
    const rowOffsets = [0];
    for (let row = 1; row < 20; row += 1) rowOffsets.push(row, -row);
    for (const row of rowOffsets) {
      const candidate = { x: source.x + column * columnStep, y: source.y + row * rowStep };
      const overlaps = nodes.some(
        (node) =>
          candidate.x < node.x + NODE_WIDTH &&
          candidate.x + NODE_WIDTH > node.x &&
          candidate.y < node.y + NODE_HEIGHT &&
          candidate.y + NODE_HEIGHT > node.y,
      );
      if (!overlaps) return candidate;
    }
  }
  return { x: source.x + columnStep, y: source.y };
}
