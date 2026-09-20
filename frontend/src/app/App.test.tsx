import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';

const project = {
  session_id: 'path-test-a',
  database_path: 'tmp/projects/path-test.duckdb',
  domain: 'core',
  metadata: {},
};

vi.mock('../backend/StreamFindApiClient', () => ({
  StreamFindApiClient: class {
    async connect(onState: (state: 'connecting' | 'ready') => void) {
      onState('connecting');
      onState('ready');
      return { service: 'streamfind_service', protocol_version: '1.0', state: 'ready', backend_version: 'test' };
    }

    async capabilities() {
      return { protocol_version: '1.0', operations: [], domains: ['core'], endpoints: [] };
    }

    async projects() {
      return [project];
    }

    async closeProject() {
      return project;
    }

    async workflowState() {
      return { session_id: project.session_id, state: 'idle' as const };
    }

    async validateWorkflow() {
      return { session_id: project.session_id, state: 'validated' as const, valid: true };
    }

    async runWorkflow() {
      return { session_id: project.session_id, state: 'queued' as const };
    }

    async pauseWorkflow() {
      return { session_id: project.session_id, state: 'paused' as const };
    }

    async cancelWorkflow() {
      return { session_id: project.session_id, state: 'cancelled' as const };
    }

    subscribe() {
      return () => undefined;
    }

    disconnect() {
      return undefined;
    }
  },
}));

describe('application shell', () => {
  beforeEach(() => {
    localStorage.clear();
    window.location.hash = '';
    vi.useFakeTimers();
  });

  afterEach(() => {
    cleanup();
    vi.useRealTimers();
  });

  async function showWorkspace() {
    render(<App />);
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    await act(async () => {
      vi.advanceTimersByTime(3_000);
    });
    expect(screen.getByTestId('bootstrap-splash')).toHaveClass('sf-splash-exit');
    await act(async () => {
      vi.advanceTimersByTime(220);
      await Promise.resolve();
    });
    expect(screen.getByText('Create project')).toBeInTheDocument();
  }

  it('keeps the splash gate visible before the workspace and fades into Project Hub', async () => {
    render(<App />);
    expect(screen.getByText('Initializing workspace')).toBeInTheDocument();

    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
      vi.advanceTimersByTime(3_000);
    });
    expect(screen.getByTestId('bootstrap-splash')).toHaveClass('sf-splash-exit');
    await act(async () => {
      vi.advanceTimersByTime(220);
      await Promise.resolve();
    });

    expect(screen.getByText('Create project')).toBeInTheDocument();
  });

  it('renders fixed project actions and discovered project cards', async () => {
    await showWorkspace();

    expect(screen.getByText('Open project')).toBeInTheDocument();
    expect(screen.getByText('path-test')).toBeInTheDocument();
    expect(screen.getByText('tmp/projects/path-test.duckdb')).toBeInTheDocument();
  });

  it('keeps the Project Hub visible when no project session is active', async () => {
    window.location.hash = '#/workflow';
    await showWorkspace();

    expect(screen.getByText('Create project')).toBeInTheDocument();
    expect(screen.queryByText('Project workspace')).not.toBeInTheDocument();
  });

  it('writes and restores a session-aware project route', async () => {
    await showWorkspace();
    await act(async () => {
      screen.getByRole('button', { name: 'Open workflow canvas for path-test' }).click();
    });

    expect(window.location.hash).toBe('#/project/path-test-a/workflow');
    expect(screen.getByText('Workflow canvas')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Connect from path-test' })).not.toBeInTheDocument();

    cleanup();
    window.location.hash = '#/project/path-test-a/workflow';
    render(<App />);
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
      vi.advanceTimersByTime(3_000);
    });
    await act(async () => {
      vi.advanceTimersByTime(220);
      await Promise.resolve();
    });

    expect(screen.getByText('Workflow canvas')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Connect from path-test' })).not.toBeInTheDocument();
  });

  it('persists an appearance choice from Settings', async () => {
    await showWorkspace();

    await act(async () => {
      screen.getByRole('button', { name: 'Open appearance settings' }).click();
    });
    await act(async () => {
      screen.getByRole('button', { name: /Dark.*Low-luminance workbench/ }).click();
    });

    expect(document.documentElement.dataset.theme).toBe('dark');
    expect(localStorage.getItem('streamfind.theme')).toBe('dark');
  });

  it('disconnects a project from the workspace without opening it', async () => {
    await showWorkspace();

    await act(async () => {
      screen.getByRole('button', { name: 'Close project path-test' }).click();
      await Promise.resolve();
    });

    expect(screen.queryByText('path-test')).not.toBeInTheDocument();
    expect(window.location.hash).toBe('');
  });
});
