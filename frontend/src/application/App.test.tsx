import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';

let project = {
  session_id: 'path-test-a',
  project_path: 'tmp/projects/path-test.sf',
  database_size_bytes: 0,
  domains: ['core'],
  metadata: {},
  result_store: { size_bytes: 0 },
};
let publishedArtifactCount = 0;
let serviceEventListener: ((event: { type: string; project?: string }) => void) | null = null;
let createProjectCalls: Array<Record<string, unknown>> = [];
let copyProjectCalls: Array<{ sessionId: string; destinationPath: string }> = [];
let createProjectError: Error | null = null;
let browsedFolders: Array<{ name: string; path: string; isDirectory: boolean }> = [];
let nativeFolderPickerPaths: string[] = [];
let nativeFolderPickerError: Error | null = null;

vi.mock('../framework/backend/StreamFindApiClient', () => ({
  StreamFindApiClient: class {
    async connect(onState: (state: 'connecting' | 'ready') => void) {
      onState('connecting');
      onState('ready');
      return { service: 'streamfind_service', protocol_version: '1.0', state: 'ready', backend_version: 'test' };
    }

    async capabilities() {
      return { protocol_version: '1.0', operations: [], domains: ['core'], endpoints: [] };
    }

    async workflowDemos() {
      return [
        {
          id: 'demo',
          name: 'Demo workflow',
          description: 'A demo workflow.',
          use_case: 'Show the workflow demo picker.',
          workflow: {
            schema_version: 2 as const,
            workflow_id: 'demo',
            name: 'Demo workflow',
            version: 1,
            definition_revision: 1,
            metadata: {
              name: 'Demo workflow',
              description: 'A demo workflow.',
              use_case: 'Show the workflow demo picker.',
            },
            operations: [],
            connections: [],
          },
        },
      ];
    }

    async saveWorkflow() {
      return {
        workflow: {
          schema_version: 2 as const,
          workflow_id: 'demo',
          version: 1,
          definition_revision: 1,
          operations: [],
          connections: [],
        },
        valid: true,
        diagnostics: [],
      };
    }

    async projects() {
      return [project];
    }

    async createProject(input: Record<string, unknown>) {
      createProjectCalls.push(input);
      if (createProjectError) throw createProjectError;
      return {
        ...project,
        session_id: input.session_id as string,
        project_path: input.project_path as string,
      };
    }

    async copyProject(sessionId: string, destinationPath: string) {
      copyProjectCalls.push({ sessionId, destinationPath });
    }

    async browseFileSystem(path = '') {
      return { path: path || 'C:/projects', entries: browsedFolders };
    }

    async pickFolders() {
      if (nativeFolderPickerError) throw nativeFolderPickerError;
      return nativeFolderPickerPaths;
    }

    async results() {
      return Array.from({ length: publishedArtifactCount }, (_, index) => ({
        result_id: `result-${index}`,
        status: 'published',
      }));
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

    async cancelWorkflow() {
      return { session_id: project.session_id, state: 'cancelled' as const };
    }

    subscribe(listener: (event: { type: string; project?: string }) => void) {
      serviceEventListener = listener;
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
    project = {
      session_id: 'path-test-a',
      project_path: 'tmp/projects/path-test.sf',
      database_size_bytes: 0,
      domains: ['core'],
      metadata: {},
      result_store: { size_bytes: 0 },
    };
    publishedArtifactCount = 0;
    serviceEventListener = null;
    createProjectCalls = [];
    copyProjectCalls = [];
    createProjectError = null;
    nativeFolderPickerPaths = [];
    nativeFolderPickerError = new Error('Native folder picker unavailable in test environment.');
    browsedFolders = [
      { name: 'existing.sf', path: 'C:/projects/existing.sf', isDirectory: true },
      { name: 'ordinary-folder', path: 'C:/projects/ordinary-folder', isDirectory: true },
      { name: 'notes.txt', path: 'C:/projects/notes.txt', isDirectory: false },
    ];
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
    expect(screen.getByText('Create workflow')).toBeInTheDocument();
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

    expect(screen.getByText('Create workflow')).toBeInTheDocument();
  });

  it('lists registered workflow demos from the backend', async () => {
    await showWorkspace();
    expect(screen.getByText('Workflow demos')).toBeInTheDocument();
    await act(async () => {
      screen.getByRole('button', { name: /Workflow demos/ }).click();
      await Promise.resolve();
    });
    expect(screen.getByText('Demo workflow')).toBeInTheDocument();
    expect(
      screen.queryByText('Show the workflow demo picker.', { selector: '.sf-demo-list-item small' }),
    ).not.toBeInTheDocument();
    await act(async () => {
      screen.getByRole('button', { name: /Demo workflow/ }).click();
    });
    expect(screen.getByRole('heading', { name: 'Demo workflow' })).toBeInTheDocument();
    expect(screen.getByText('A demo workflow.')).toBeInTheDocument();
    expect(screen.getByText('0 operations')).toBeInTheDocument();
    expect(screen.queryByText(/Revision/)).not.toBeInTheDocument();
    expect(screen.queryByText(/workflow demo/, { selector: '.sf-workflow-summary' })).not.toBeInTheDocument();
    expect(
      screen.queryByText('Show the workflow demo picker.', { selector: '.sf-project-preview-domain' }),
    ).not.toBeInTheDocument();
    expect(screen.getByText(/"use_case": "Show the workflow demo picker\."/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Create Workflow' })).toBeInTheDocument();
    await act(async () => {
      screen.getByRole('button', { name: 'Create Workflow' }).click();
    });
    expect((screen.getByLabelText('Workflow metadata JSON') as HTMLTextAreaElement).value).toContain(
      '"name": "Demo workflow"',
    );
    await act(async () => {
      screen.getByRole('button', { name: 'Cancel' }).click();
      await Promise.resolve();
    });
    expect(screen.getByRole('heading', { name: 'Demo workflow' })).toBeInTheDocument();
    await act(async () => {
      screen.getByRole('button', { name: 'Close workflow demo preview' }).click();
    });
    await act(async () => {
      screen.getByRole('button', { name: 'Create workflow' }).click();
    });
    expect((screen.getByLabelText('Workflow metadata JSON') as HTMLTextAreaElement).value).toContain(
      '"name": "Untitled workflow"',
    );
  });

  it('renders fixed project actions and discovered project cards', async () => {
    await showWorkspace();

    expect(screen.getByText('Open workflow')).toBeInTheDocument();
    expect(screen.getByText('path-test')).toBeInTheDocument();
    expect(screen.getByText('tmp/projects/path-test.sf')).toBeInTheDocument();
  });

  it('previews the .sf destination and submits it when creating a project', async () => {
    createProjectError = new Error('Simulated service refusal.');
    await showWorkspace();
    fireEvent.click(screen.getByRole('button', { name: 'Create workflow' }));
    fireEvent.change(screen.getByPlaceholderText('C:/projects'), { target: { value: 'C:/projects' } });
    fireEvent.change(screen.getByPlaceholderText('river-screening'), { target: { value: 'river-screening' } });

    expect(screen.getByText('C:/projects/river-screening.sf')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Create project' }));
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(createProjectCalls[0]).toMatchObject({
      project_path: 'C:/projects/river-screening.sf',
      mode: 'create',
    });
    expect(screen.getByRole('alert')).toHaveTextContent('Simulated service refusal.');
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('heading', { name: 'Create project' })).not.toBeInTheDocument();
  });

  it('opens a selected .sf folder and keeps the open dialog when the picker is cancelled', async () => {
    await showWorkspace();
    fireEvent.click(screen.getByRole('button', { name: 'Open workflow' }));
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: 'Browse' }));
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(screen.getByRole('button', { name: /existing\.sf/ })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /ordinary-folder/ })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /notes\.txt/ })).not.toBeInTheDocument();
    fireEvent.click(within(screen.getByRole('dialog', { name: 'Choose project folder' })).getByRole('button', { name: 'Cancel' }));
    expect(screen.getByRole('heading', { name: 'Open project' })).toBeInTheDocument();

    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: 'Browse' }));
      await Promise.resolve();
      await Promise.resolve();
    });
    fireEvent.click(screen.getByRole('button', { name: /existing\.sf/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Open selected project' }));
    expect((screen.getByLabelText('Project folder (.sf)') as HTMLInputElement).value).toBe('C:/projects/existing.sf');
    fireEvent.click(screen.getByRole('button', { name: 'Open project' }));

    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(createProjectCalls).toHaveLength(1);
    expect(createProjectCalls[0]).toMatchObject({ project_path: 'C:/projects/existing.sf', mode: 'open' });
    expect(screen.getByText('existing')).toBeInTheDocument();
  });

  it('uses the native folder wizard for open workflow when it is available', async () => {
    nativeFolderPickerError = null;
    nativeFolderPickerPaths = ['C:/projects/native-existing.sf'];
    await showWorkspace();
    fireEvent.click(screen.getByRole('button', { name: 'Open workflow' }));
    fireEvent.click(screen.getByRole('button', { name: 'Browse' }));

    await act(async () => { await Promise.resolve(); });
    expect((screen.getByLabelText('Project folder (.sf)') as HTMLInputElement).value).toBe(
      'C:/projects/native-existing.sf',
    );
    expect(screen.queryByRole('dialog', { name: 'Choose project folder' })).not.toBeInTheDocument();
  });

  it('shows the service guidance when a legacy DuckDB file is entered as a project folder', async () => {
    createProjectError = new Error('A legacy DuckDB project file cannot be opened as a current project. Create a new .sf project.');
    await showWorkspace();
    fireEvent.click(screen.getByRole('button', { name: 'Open workflow' }));
    fireEvent.change(screen.getByLabelText('Project folder (.sf)'), { target: { value: 'C:/projects/legacy.duckdb' } });
    fireEvent.click(screen.getByRole('button', { name: 'Open project' }));

    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(createProjectCalls[0]).toMatchObject({ project_path: 'C:/projects/legacy.duckdb', mode: 'open' });
    expect(screen.getByRole('alert')).toHaveTextContent('Create a new .sf project');
  });

  it('copies a project to a new .sf folder and opens the copy', async () => {
    await showWorkspace();
    fireEvent.click(screen.getByRole('button', { name: 'Copy project path-test' }));

    expect(screen.getByText('tmp/projects/path-test-copy.sf')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Copy project' }));
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });

    expect(copyProjectCalls).toEqual([{ sessionId: 'path-test-a', destinationPath: 'tmp/projects/path-test-copy.sf' }]);
    expect(createProjectCalls[0]).toMatchObject({ project_path: 'tmp/projects/path-test-copy.sf', mode: 'open' });
    expect(screen.getByText('path-test-copy')).toBeInTheDocument();
  });

  it('shows the workflow state when the definition request is temporarily blocked', async () => {
    await showWorkspace();

    expect(screen.getByText('path-test')).toBeInTheDocument();
    expect(screen.getByText('DuckDB: 0.0 KB')).toBeInTheDocument();
    expect(screen.getByText('Results store: 0.0 KB')).toBeInTheDocument();
    expect(screen.queryByText(/Workflow (queued|running|failed)/)).not.toBeInTheDocument();
    expect(screen.queryByText('Loading workflow summary…')).not.toBeInTheDocument();
  });

  it('refreshes project card result counts and store size after workflow events', async () => {
    await showWorkspace();
    expect(screen.getByText('Results store: 0.0 KB')).toBeInTheDocument();

    project = { ...project, result_store: { size_bytes: 2048 } };
    publishedArtifactCount = 2;
    await act(async () => {
      serviceEventListener?.({ type: 'workflow.completed', project: project.session_id });
      await Promise.resolve();
      await Promise.resolve();
    });
    await act(async () => {
      vi.advanceTimersByTime(1_600);
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(screen.getByText('Results: 2 published')).toBeInTheDocument();
    expect(screen.getByText('Results store: 2.0 KB')).toBeInTheDocument();
  });

  it('keeps the Project Hub visible when no project session is active', async () => {
    window.location.hash = '#/workflow';
    await showWorkspace();

    expect(screen.getByText('Create workflow')).toBeInTheDocument();
    expect(screen.queryByText('Project workspace')).not.toBeInTheDocument();
  });

  it('writes and restores a session-aware project route', async () => {
    await showWorkspace();
    await act(async () => {
      screen.getByRole('button', { name: 'Open workflow canvas for path-test' }).click();
    });

    expect(window.location.hash).toBe('#/project/path-test-a/workflow');
    expect(screen.getByRole('log', { name: 'Canvas activity log' })).toBeInTheDocument();
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

    expect(screen.getByRole('log', { name: 'Canvas activity log' })).toBeInTheDocument();
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
