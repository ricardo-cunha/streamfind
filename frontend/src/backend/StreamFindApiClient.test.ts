import { afterEach, describe, expect, it, vi } from 'vitest';
import { StreamFindApiClient } from './StreamFindApiClient';

const response = (body: unknown, ok = true, status = 200) => ({ ok, status, json: async () => body }) as Response;

describe('StreamFindApiClient capability discovery', () => {
  afterEach(() => vi.restoreAllMocks());

  it('loads the lightweight capability index', async () => {
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValue(
        response({ protocol_version: '1.0', domains: ['core'], modules: [{ domain: 'core', module_id: 'io' }] }),
      );

    const result = await new StreamFindApiClient('http://service').capabilitiesIndex();

    expect(fetchMock).toHaveBeenCalledWith('http://service/capabilities/index');
    expect(result.modules[0].module_id).toBe('io');
  });

  it('encodes domain and applies module and search filters for operation summaries', async () => {
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValue(
        response({
          operations: [
            { canonical_id: 'core:io/read', label: 'Read', domain: 'lab/core', module_id: 'io', definition: 'Read' },
          ],
        }),
      );

    const result = await new StreamFindApiClient('http://service').capabilityOperations({
      domain: 'lab/core',
      module: 'io',
      search: 'read',
    });

    expect(fetchMock).toHaveBeenCalledWith(
      'http://service/capabilities/operations?domain=lab%2Fcore&module=io&search=read',
    );
    expect(result[0].canonical_id).toBe('core:io/read');
  });

  it('loads domain modules and a complete operation by canonical id', async () => {
    const operation = { canonical_id: 'core:io/read', kind: 'operation' };
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(response({ domain: 'core', modules: ['io'] }))
      .mockResolvedValueOnce(response(operation));
    const client = new StreamFindApiClient('http://service');

    await expect(client.capabilityModules('core')).resolves.toEqual({ domain: 'core', modules: ['io'] });
    await expect(client.capabilityOperation('core:io/read')).resolves.toEqual(operation);
    expect(fetchMock).toHaveBeenNthCalledWith(1, 'http://service/capabilities/domains/core/modules');
    expect(fetchMock).toHaveBeenNthCalledWith(2, 'http://service/capabilities/operations/core%3Aio%2Fread');
  });

  it('reports capability endpoint failures', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(response({}, false, 503));

    await expect(new StreamFindApiClient('http://service').capabilitiesIndex()).rejects.toThrow(
      'Capabilities index request failed (503)',
    );
  });
});
