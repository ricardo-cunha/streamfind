import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { StreamFindApiClient, type ArtifactRecord } from '../../../framework/backend/StreamFindApiClient';
import type { FrontendPluginApi } from '../../../framework/plugins/pluginTypes';
import { GenericTableViewer } from './GenericTableViewer';

afterEach(() => cleanup());

const artifact: ArtifactRecord = {
  artifact_id: 'artifact-table',
  contract_id: 'table',
  representation: 'table',
  columns: [{ name: 'value', type: 'INTEGER' }],
  producer_operation: 'operation',
  producer_instance: 'instance',
  workflow_revision: 1,
  status: 'published',
};

describe('GenericTableViewer', () => {
  it('loads the first page and requests the next page through the typed client', async () => {
    const client = new StreamFindApiClient('http://service');
    const artifactData = vi.spyOn(client, 'artifactData');
    artifactData
      .mockResolvedValueOnce({
        artifact_id: artifact.artifact_id,
        columns: artifact.columns ?? [],
        rows: [{ value: '1' }],
        offset: 0,
        limit: 250,
        total_rows: 251,
      })
      .mockResolvedValueOnce({
        artifact_id: artifact.artifact_id,
        columns: artifact.columns ?? [],
        rows: [{ value: '2' }],
        offset: 250,
        limit: 250,
        total_rows: 251,
      });
    const pluginApi = { client } as FrontendPluginApi;

    render(
      <GenericTableViewer
        context={{
          sessionId: 'session',
          artifactId: artifact.artifact_id,
          semanticType: artifact.contract_id,
          artifact,
          pluginApi,
        }}
      />,
    );

    await waitFor(() => expect(artifactData).toHaveBeenCalledWith('session', expect.objectContaining({ offset: 0 })));
    expect(screen.getByRole('button', { name: 'Next page' })).toBeEnabled();

    fireEvent.click(screen.getByRole('button', { name: 'Next page' }));

    await waitFor(() => expect(artifactData).toHaveBeenCalledWith('session', expect.objectContaining({ offset: 250 })));
  });
});
