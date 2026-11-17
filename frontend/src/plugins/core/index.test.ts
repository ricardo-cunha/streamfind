import { describe, expect, it } from 'vitest';
import { FrontendPluginRegistry } from '../../framework/plugins/FrontendPluginRegistry';
import { ViewerRegistry } from '../../framework/viewers/viewerTypes';
import { registerCoreFrontendPlugin } from './index';

describe('core frontend plugin', () => {
  it('registers the generic table viewer', () => {
    const viewers = new ViewerRegistry();
    const registry = new FrontendPluginRegistry(viewers);

    registerCoreFrontendPlugin(registry);

    expect(registry.ids()).toEqual(['streamfind.core']);
    expect(viewers.get('core.table')?.label).toBe('Table renderer');
    expect(viewers.resolve({ semanticType: 'sf:featuresTable', representation: 'table' })?.id).toBe('core.table');
  });
});
