import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ViewerShell } from './ViewerShell';

afterEach(() => cleanup());

describe('ViewerShell', () => {
  it('provides an application-owned dialog shell and closes from the backdrop', () => {
    const onClose = vi.fn();
    render(
      <ViewerShell title="Visualization artifact" subtitle="artifact-a" onClose={onClose} variant="visualization">
        <div>Interactive viewer</div>
      </ViewerShell>,
    );

    expect(screen.getByRole('dialog', { name: 'Visualization artifact' })).toHaveClass('visualization');
    expect(screen.getByText('Interactive viewer')).toBeInTheDocument();
    screen.getByRole('presentation').dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    expect(onClose).toHaveBeenCalledOnce();
  });
});
