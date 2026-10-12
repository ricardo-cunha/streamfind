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
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
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

  it('loads validated workflow demos from the service', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response({
        workflows: [
          {
            id: 'demo',
            name: 'Demo',
            description: 'Example',
            workflow: { schema_version: 1, version: 1, operations: [], connections: [] },
          },
        ],
      }),
    );

    const result = await new StreamFindApiClient('http://service').workflowDemos();

    expect(fetchMock).toHaveBeenCalledWith('http://service/workflow-demos');
    expect(result[0].id).toBe('demo');
  });

  it('reports capability endpoint failures', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(response({}, false, 503));

    await expect(new StreamFindApiClient('http://service').capabilitiesIndex()).rejects.toThrow(
      'Capabilities index request failed (503)',
    );
  });

  it('preserves the backend error body for operation failures', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response({ error: 'Workflow execution lock was lost' }, false, 400),
    );

    await expect(
      new StreamFindApiClient('http://service').runOperation('session', 'mass_spec.read_mass_spec_files', {}),
    ).rejects.toThrow('Operation request failed (400): Workflow execution lock was lost');
  });

  it('queries bounded artifact data through the generic query endpoint', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response({
        artifact_id: 'features-1',
        columns: [{ name: 'rt', type: 'DOUBLE' }],
        rows: [{ rt: '12.5' }],
        offset: 0,
        limit: 50000,
        total_rows: 1000000,
        returned_rows: 1,
        mode: 'sample',
        has_more: true,
      }),
    );

    const result = await new StreamFindApiClient('http://service').artifactQuery('session/1', {
      artifact_id: 'features-1',
      mode: 'sample',
      columns: ['rt', 'mz'],
      x_column: 'rt',
      y_column: 'mz',
      limit: 50000,
    });

    expect(fetchMock).toHaveBeenCalledWith('http://service/projects/session%2F1/artifacts/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        artifact_id: 'features-1',
        mode: 'sample',
        columns: ['rt', 'mz'],
        x_column: 'rt',
        y_column: 'mz',
        limit: 50000,
      }),
    });
    expect(result.mode).toBe('sample');
    expect(result.total_rows).toBe(1000000);
  });
});
