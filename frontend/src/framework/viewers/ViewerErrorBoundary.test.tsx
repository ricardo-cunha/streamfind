import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ReactNode } from 'react';
import { ViewerErrorBoundary } from './ViewerErrorBoundary';

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

describe('ViewerErrorBoundary', () => {
  it('isolates a viewer render failure to the viewer surface', () => {
    const error = new Error('renderer exploded');
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    function BrokenViewer(): ReactNode {
      throw error;
    }

    render(
      <ViewerErrorBoundary artifactId="artifact-1">
        <BrokenViewer />
      </ViewerErrorBoundary>,
    );

    expect(screen.getByRole('alert')).toHaveTextContent('Viewer failed');
    expect(screen.getByText('renderer exploded')).toBeInTheDocument();
    expect(consoleError).toHaveBeenCalled();
  });
});
