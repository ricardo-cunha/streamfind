import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ArtifactRecord } from '../backend/StreamFindApiClient';
import { ArtifactViewerHost } from './ArtifactViewerHost';

afterEach(() => cleanup());

const artifact: ArtifactRecord = {
  artifact_id: 'artifact-1',
  contract_id: 'sf:result',
  representation: 'json',
  producer_operation: 'sf:run',
  producer_instance: 'operation-1',
  workflow_revision: 1,
  status: 'published',
};

describe('ArtifactViewerHost', () => {
  it('renders the fallback directly for default JSON artifacts', () => {
    const onClose = vi.fn();
    render(
      <ArtifactViewerHost
        artifact={artifact}
        sessionId="session-1"
        onClose={onClose}
        fallback={<div>JSON fallback</div>}
      />,
    );
    expect(screen.getByRole('dialog', { name: 'sf:result' })).toHaveClass('json');
    expect(screen.getByText('JSON fallback')).toBeInTheDocument();
  });

  it('uses the wide shell and resolver path for table mode', () => {
    const tableArtifact = { ...artifact, representation: 'table' };
    render(
      <ArtifactViewerHost
        artifact={tableArtifact}
        sessionId="session-1"
        mode="table"
        onClose={() => undefined}
        fallback={<div>Table fallback</div>}
      />,
    );
    expect(screen.getByRole('dialog', { name: 'sf:result' })).toHaveClass('wide');
    expect(screen.getByText('Table fallback')).toBeInTheDocument();
  });
});
