import { afterEach, describe, expect, it, vi } from 'vitest';
import { StreamFindApiClient } from './StreamFindApiClient';

const response = (body: unknown, ok = true, status = 200) => ({ ok, status, json: async () => body }) as Response;

describe('StreamFindApiClient capability discovery', () => {
  afterEach(() => vi.restoreAllMocks());

  it('loads the lightweight capability index', async () => {
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValue(
        response({ protocol_version: '1.0', plugins: ['core'], modules: [{ plugin: 'core', module_id: 'io' }] }),
      );

    const result = await new StreamFindApiClient('http://service').capabilitiesIndex();

    expect(fetchMock).toHaveBeenCalledWith('http://service/capabilities/index');
    expect(result.modules[0].module_id).toBe('io');
  });

  it('encodes plugin and applies module and search filters for operation summaries', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response({
        operations: [
          { canonical_id: 'core:io/read', label: 'Read', plugin: 'lab/core', module_id: 'io', definition: 'Read' },
        ],
      }),
    );

    const result = await new StreamFindApiClient('http://service').capabilityOperations({
      plugin: 'lab/core',
      module: 'io',
      search: 'read',
    });

    expect(fetchMock).toHaveBeenCalledWith(
      'http://service/capabilities/operations?plugin=lab%2Fcore&module=io&search=read',
    );
    expect(result[0].canonical_id).toBe('core:io/read');
  });

  it('loads plugin modules and a complete operation by canonical id', async () => {
    const operation = { canonical_id: 'core:io/read', kind: 'operation' };
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(response({ plugin: 'core', modules: ['io'] }))
      .mockResolvedValueOnce(response(operation));
    const client = new StreamFindApiClient('http://service');

    await expect(client.capabilityModules('core')).resolves.toEqual({ plugin: 'core', modules: ['io'] });
    await expect(client.capabilityOperation('core:io/read')).resolves.toEqual(operation);
    expect(fetchMock).toHaveBeenNthCalledWith(1, 'http://service/capabilities/plugins/core/modules');
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
            workflow: { schema_version: 2, version: 1, definition_revision: 1, operations: [], connections: [] },
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

  it('surfaces concurrent workflow save conflicts from the service', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response(
        {
          code: 'workflow_revision_conflict',
          error: 'workflow definition revision conflict: expected 3, current revision is 4; reload before saving',
        },
        false,
        409,
      ),
    );

    await expect(
      new StreamFindApiClient('http://service').saveWorkflow('session', {
        schema_version: 2,
        version: 2,
        definition_revision: 3,
        metadata: {},
        operations: [],
        connections: [],
      }),
    ).rejects.toThrow('workflow definition revision conflict: expected 3, current revision is 4');
  });

  it('queries bounded artifact data through the generic query endpoint', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      response({
        result_id: 'features-1',
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

    const result = await new StreamFindApiClient('http://service').resultQuery('session/1', {
      result_id: 'features-1',
      mode: 'sample',
      columns: ['rt', 'mz'],
      x_column: 'rt',
      y_column: 'mz',
      limit: 50000,
    });

    expect(fetchMock).toHaveBeenCalledWith('http://service/projects/session%2F1/results/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        result_id: 'features-1',
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

  it('loads a selected JSON payload without parsing string values', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(response({ result_id: 'json-1', payload: '42' }));

    await expect(new StreamFindApiClient('http://service').resultPayload('session-1', 'json-1')).resolves.toBe('42');
    expect(fetchMock).toHaveBeenCalledWith('http://service/projects/session-1/results/data', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ result_id: 'json-1' }),
    });
  });

  it('preserves explicit JSON null and rejects a mismatched result payload', async () => {
    vi.spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(response({ result_id: 'json-1', payload: null }))
      .mockResolvedValueOnce(response({ result_id: 'other', payload: {} }));
    const client = new StreamFindApiClient('http://service');

    await expect(client.resultPayload('session-1', 'json-1')).resolves.toBeNull();
    await expect(client.resultPayload('session-1', 'json-1')).rejects.toThrow(
      'Result payload response did not match the requested result',
    );
  });
});
