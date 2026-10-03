import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { StartupFailureScreen } from './StartupFailureScreen';

afterEach(() => cleanup());

describe('StartupFailureScreen', () => {
  it('renders actionable startup failure details', () => {
    render(<StartupFailureScreen error={new Error('Backend initialization failed.')} />);
    expect(screen.getByRole('alert')).toHaveTextContent('Backend initialization failed.');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  });

  it('falls back to a generic message for unknown failures', () => {
    render(<StartupFailureScreen error={{ reason: 'unknown' }} />);
    expect(screen.getByRole('alert')).toHaveTextContent('The frontend could not start.');
  });
});
