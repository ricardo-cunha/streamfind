import { describe, expect, it } from 'vitest';
import { registerCoreViewers } from './registerCoreViewers';
import { ViewerRegistry } from './viewerTypes';

describe('core viewers', () => {
  it('registers the visualization artifact viewer by semantic contract', () => {
    const registry = new ViewerRegistry();

    registerCoreViewers(registry);

    expect(registry.ids()).toContain('core.visualization');
    expect(registry.resolve('sfvis:visualizationSpecResult')?.id).toBe('core.visualization');
  });
});
