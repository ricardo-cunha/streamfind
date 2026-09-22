import { Component, type ErrorInfo, type FormEvent, type ReactNode, useEffect, useMemo, useState } from 'react';
import logo from '../assets/streamfind.png';

import WorkflowCanvas from './WorkflowCanvas';
import { NotificationToasts } from './NotificationCenter';
import { StreamFindApiClient, type ProjectSession, type ServiceState } from '../backend/StreamFindApiClient';
import type { ServiceCapabilities, WorkflowState } from '../backend/protocol';
import { notifyApp } from './notifications';

type ThemeMode = 'light' | 'dark';
type Palette = 'streamfind' | 'playful' | 'matrix';
type Style = 'classic' | 'studio' | 'chrome';
type RouteKey = 'projects' | 'workflow' | 'results' | 'data' | 'runs' | 'provenance';
type ParsedRoute = { route: RouteKey; sessionId?: string };
type NavItem = { key: RouteKey; label: string; icon: string; hint: string };
type WorkflowSummary = {
  operationCount: number;
  revision: number;
  state: WorkflowState;
  artifactCount: number;
};

function projectNameFromPath(databasePath: string): string {
  return (
    databasePath
      .split(/[\\/]/)
      .pop()
      ?.replace(/\.[^.]+$/, '') || 'project'
  );
}

function forceDuckDbPath(databasePath: string): string {
  const normalized = databasePath.trim();
  if (!normalized) return normalized;
  return /\.duckdb$/i.test(normalized) ? normalized : normalized.replace(/\.[^.\\/]+$/, '') + '.duckdb';
}

function uniqueSessionId(projectName: string, projects: ProjectSession[]): string {
  const existing = new Set(projects.map((project) => project.session_id));
  if (!existing.has(projectName)) return projectName;
  let prefix = 1;
  while (existing.has(`${prefix}_${projectName}`)) prefix += 1;
  return `${prefix}_${projectName}`;
}

const navItems: NavItem[] = [
  { key: 'projects', label: 'Project Hub', icon: 'fa-solid fa-house', hint: 'Create and open projects' },
  {
    key: 'workflow',
    label: 'Workflow',
    icon: 'fa-solid fa-diagram-project',
    hint: 'Compose ontology-driven workflows',
  },

  { key: 'results', label: 'Results', icon: 'fa-solid fa-wave-square', hint: 'Explore scientific outputs' },
  { key: 'data', label: 'Data', icon: 'fa-solid fa-boxes-stacked', hint: 'Inspect project tables' },
  { key: 'runs', label: 'Runs', icon: 'fa-solid fa-clock', hint: 'Monitor durable executions' },
  { key: 'provenance', label: 'Provenance', icon: 'fa-solid fa-book-open', hint: 'Trace results to their source' },
];
const palettes = [
  {
    id: 'streamfind' as Palette,
    label: 'streamfind',
    description: 'White/black foundation with navy, green, and aqua accents.',
    swatches: ['#08296c', '#4c8333', '#78a7ff', '#83b95a'],
  },
  {
    id: 'playful' as Palette,
    label: 'Playful',
    description: 'White/black foundation with orange, blue, and green accents.',
    swatches: ['#f07848', '#2864dc', '#78a7ff', '#67d391'],
  },
  {
    id: 'matrix' as Palette,
    label: 'Matrix',
    description: 'White/black foundation with neon-green and lime accents.',
    swatches: ['#168a46', '#50e878', '#6c9f28', '#b6f36b'],
  },
];
const styles = [
  {
    id: 'classic' as Style,
    label: 'Classic',
    description: 'Flat panels, sharp edges, and compact administration density.',
  },
  {
    id: 'studio' as Style,
    label: 'Studio',
    description: 'Editor-like proportions with restrained glow and focused workspaces.',
  },
  { id: 'chrome' as Style, label: 'Chrome', description: 'Rounded browser-inspired framing with polished navigation.' },
];
function routeFromHash(): ParsedRoute {
  const parts = window.location.hash.replace(/^#\/?/, '').split('/').filter(Boolean);
  if (parts[0] === 'project' && parts[1]) {
    const route = parts[2] as RouteKey;
    return {
      route: navItems.some((item) => item.key === route && item.key !== 'projects') ? route : 'workflow',
      sessionId: decodeURIComponent(parts[1]),
    };
  }
  const route = parts[0] as RouteKey;
  return { route: navItems.some((item) => item.key === route) ? route : 'projects' };
}
function navigateHash(key: RouteKey, sessionId?: string): void {
  const path = sessionId ? `#/project/${encodeURIComponent(sessionId)}/${key}` : `#/${key}`;
  window.history.pushState({}, '', path);
  window.dispatchEvent(new Event('hashchange'));
}
function ErrorFallback({ error }: { error: Error }) {
  return (
    <div className="sf-error">
      <strong>streamfind could not render this view.</strong>
      <span>{error.message}</span>
    </div>
  );
}
class ErrorBoundary extends Component<{ children: ReactNode }, { error: Error | null }> {
  state = { error: null as Error | null };
  static getDerivedStateFromError(error: Error) {
    return { error };
  }
  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('streamfind UI error', error, info);
  }
  render() {
    return this.state.error ? <ErrorFallback error={this.state.error} /> : this.props.children;
  }
}
function BootstrapGate({
  children,
  client,
  onState,
}: {
  children: ReactNode;
  client: StreamFindApiClient;
  onState: (state: ServiceState) => void;
}) {
  const [state, setState] = useState<'loading' | 'exiting' | 'ready' | 'failed'>('loading');
  const [retryKey, setRetryKey] = useState(0);
  useEffect(() => {
    let active = true;
    let timer: number | undefined;
    const minimumReadyAt = Date.now() + 3000;
    const finish = (nextState: 'exiting' | 'failed') => {
      const delay = Math.max(0, minimumReadyAt - Date.now());
      timer = window.setTimeout(() => {
        if (active) setState(nextState);
      }, delay);
    };
    void client
      .connect(onState)
      .then(() => finish('exiting'))
      .catch(() => finish('failed'));
    return () => {
      active = false;
      if (timer !== undefined) window.clearTimeout(timer);
      client.disconnect();
    };
  }, [client, onState, retryKey]);
  useEffect(() => {
    if (state !== 'exiting') return undefined;
    const timer = window.setTimeout(() => setState('ready'), 220);
    return () => window.clearTimeout(timer);
  }, [state]);
  if (state === 'loading' || state === 'exiting')
    return (
      <div data-testid="bootstrap-splash" className={`sf-splash ${state === 'exiting' ? 'sf-splash-exit' : ''}`}>
        <img src={logo} alt="streamfind" />
        <div className="sf-splash-wordmark">streamfind</div>
        <span>Initializing workspace</span>
      </div>
    );
  if (state === 'failed')
    return (
      <div data-testid="bootstrap-splash" className="sf-splash">
        <div className="sf-error">Backend initialization failed.</div>
        <button
          onClick={() => {
            setState('loading');
            setRetryKey((value) => value + 1);
          }}
        >
          Retry
        </button>
      </div>
    );
  return <>{children}</>;
}
function projectFileName(project: ProjectSession): string {
  return (
    project.database_path
      .split(/[\\/]/)
      .pop()
      ?.replace(/\.[^.]+$/, '') || project.session_id
  );
}
function formatDatabaseSize(bytes: number): string {
  const megabytes = bytes / (1024 * 1024);
  if (megabytes >= 1000) return `size: ${(megabytes / 1024).toFixed(2)} Gb`;
  return `size: ${megabytes.toFixed(2)} Mb`;
}
function ProjectCard({
  project,
  summary,
  onPreview,
  onOpenWorkflow,
  onClose,
}: {
  project: ProjectSession;
  summary?: WorkflowSummary;
  onPreview: (project: ProjectSession) => void;
  onOpenWorkflow: (project: ProjectSession) => void;
  onClose: (project: ProjectSession) => void;
}) {
  return (
    <article className="sf-project-row">
      <div>
        <strong>{projectFileName(project)}</strong>
        <span>{project.domain}</span>
        <div className="sf-workflow-summary">
          {summary ? (
            <>
              <span>
                {summary.operationCount} operations · Revision {summary.revision}
              </span>
              <span>Run: {summary.state}</span>
              <span>Artifacts: {summary.artifactCount} published</span>
            </>
          ) : (
            <span>Loading workflow summary…</span>
          )}
        </div>
        <span className="sf-workflow-size">{formatDatabaseSize(project.database_size_bytes)}</span>
        <code>{project.database_path}</code>
      </div>
      <div className="sf-project-actions">
        <button
          type="button"
          onClick={() => onPreview(project)}
          aria-label={`Preview ${projectFileName(project)}`}
          title="Preview project"
        >
          <i className="fa-solid fa-eye" />
        </button>
        <button
          type="button"
          onClick={() => onOpenWorkflow(project)}
          aria-label={`Open workflow canvas for ${projectFileName(project)}`}
          title="Open workflow canvas"
        >
          <i className="fa-solid fa-diagram-project" />
        </button>

        <button
          type="button"
          onClick={() => onClose(project)}
          aria-label={`Close project ${projectFileName(project)}`}
          title="Disconnect project"
        >
          <i className="fa-solid fa-xmark" />
        </button>
      </div>
    </article>
  );
}
function ProjectPreview({
  project,
  onOpenWorkflow,
  onClose,
}: {
  project: ProjectSession;
  onOpenWorkflow: (project: ProjectSession) => void;
  onClose: () => void;
}) {
  return (
    <div
      className="sf-dialog-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <aside className="sf-dialog sf-project-preview">
        <div className="sf-dialog-heading">
          <div>
            <span className="sf-eyebrow">Project preview</span>
            <h2>{projectFileName(project)}</h2>
          </div>
          <button type="button" className="sf-icon-button" onClick={onClose} aria-label="Close project preview">
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
        <span className="sf-project-preview-domain">{project.domain}</span>
        <code>{project.database_path}</code>
        <div className="sf-dialog-actions">
          <button type="button" className="sf-button secondary" onClick={onClose}>
            Close
          </button>
          <button type="button" className="sf-button" onClick={() => onOpenWorkflow(project)}>
            Open workflow
          </button>
        </div>
      </aside>
    </div>
  );
}
function ProjectWorkspace({
  project,
  capabilities,
  client,
  onProjectHub,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  client: StreamFindApiClient;
  onProjectHub: () => void;
}) {
  return (
    <div className="sf-page sf-project-workspace">
      <WorkflowCanvas project={project} capabilities={capabilities} client={client} onProjectHub={onProjectHub} />
    </div>
  );
}
function ProjectHub({
  serviceState,
  projects,
  client,
  onOpened,
  onAdded,
  onPreview,
  onOpenWorkflow,

  onCloseProject,
}: {
  serviceState: ServiceState;
  projects: ProjectSession[];
  client: StreamFindApiClient | null;
  onOpened: (project: ProjectSession) => void;
  onAdded: (project: ProjectSession) => void;
  onPreview: (project: ProjectSession) => void;
  onOpenWorkflow: (project: ProjectSession) => void;

  onCloseProject: (project: ProjectSession) => void;
}) {
  const [mode, setMode] = useState<'create' | 'open' | null>(null);
  const [databasePath, setDatabasePath] = useState('');
  const [domain, setDomain] = useState('core');
  const [domains, setDomains] = useState(['core']);
  const [submitting, setSubmitting] = useState(false);
  const [summaries, setSummaries] = useState<Record<string, WorkflowSummary>>({});

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setMode(null);
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => window.removeEventListener('keydown', closeOnEscape);
  }, []);

  useEffect(() => {
    client
      ?.capabilities()
      .then((capabilities) => {
        const discoveredDomains = capabilities.domains?.length ? capabilities.domains : ['core'];
        setDomains(discoveredDomains);
        setDomain((currentDomain) =>
          discoveredDomains.includes(currentDomain) ? currentDomain : (discoveredDomains[0] ?? 'core'),
        );
      })
      .catch(() => undefined);
  }, [client]);
  useEffect(() => {
    let active = true;
    if (!client) return undefined;
    Promise.all(
      projects.map(async (project) => {
        try {
          const [workflow, state, artifacts] = await Promise.all([
            client.workflowDefinition(project.session_id),
            client.workflowState(project.session_id),
            client.artifacts(project.session_id),
          ]);
          return [
            project.session_id,
            {
              operationCount: workflow.workflow.operations.length,
              revision: workflow.workflow.version,
              state: state.state,
              artifactCount: artifacts.filter((artifact) => artifact.status === 'published').length,
            },
          ] as const;
        } catch {
          return null;
        }
      }),
    ).then((entries) => {
      if (active) {
        setSummaries(
          Object.fromEntries(entries.filter((entry): entry is readonly [string, WorkflowSummary] => entry !== null)),
        );
      }
    });
    return () => {
      active = false;
    };
  }, [client, projects]);
  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!client || !mode) return;
    setSubmitting(true);
    try {
      const selectedPath = mode === 'create' ? forceDuckDbPath(databasePath) : databasePath.trim();
      const projectName = projectNameFromPath(selectedPath);
      const sessionId = mode === 'create' ? uniqueSessionId(projectName, projects) : projectName;
      const project = await client.createProject({
        session_id: sessionId,
        database_path: selectedPath,
        domain,
        mode,
      });
      (mode === 'open' ? onAdded : onOpened)(project);
      notifyApp({ kind: 'success', message: `${mode === 'create' ? 'Created' : 'Opened'} project ${projectName}.` });
      setMode(null);
    } catch (error) {
      notifyApp({ kind: 'error', message: error instanceof Error ? error.message : 'Project request failed.' });
    } finally {
      setSubmitting(false);
    }
  };
  const canInteract = serviceState === 'ready' && client !== null;
  return (
    <div className="sf-page sf-project-hub">
      <div className="sf-card-grid">
        <button
          className="sf-card action-card"
          disabled={!canInteract}
          onClick={() => {
            setDatabasePath('');
            setMode('create');
          }}
        >
          <span className="sf-card-icon">
            <i className="fa-solid fa-folder-plus" />
          </span>
          <strong>Create workflow</strong>
        </button>
        <button
          className="sf-card action-card"
          disabled={!canInteract}
          onClick={async () => {
            if (!client) return;
            setSubmitting(true);
            try {
              const path = await client.pickDatabaseFile();
              const name =
                path
                  .split(/[\\/]/)
                  .pop()
                  ?.replace(/\.[^.]+$/, '') || 'project-session';
              const project = await client.createProject({
                session_id: name,
                database_path: path,
                domain,
                mode: 'open',
              });
              onAdded(project);
              notifyApp({ kind: 'success', message: `Opened project ${name}.` });
            } catch (error) {
              notifyApp({ kind: 'error', message: error instanceof Error ? error.message : 'Project open failed.' });
            } finally {
              setSubmitting(false);
            }
          }}
        >
          <span className="sf-card-icon">
            <i className="fa-solid fa-folder-open" />
          </span>
          <strong>Open workflow</strong>
        </button>
        {projects.map((project) => (
          <ProjectCard
            key={project.session_id}
            project={project}
            summary={summaries[project.session_id]}
            onPreview={onPreview}
            onOpenWorkflow={onOpenWorkflow}
            onClose={onCloseProject}
          />
        ))}
      </div>
      {mode ? (
        <div
          className="sf-dialog-backdrop"
          onMouseDown={(event) => {
            if (event.target === event.currentTarget && !submitting) setMode(null);
          }}
        >
          <form className="sf-dialog" onSubmit={submit}>
            <div className="sf-dialog-heading">
              <div>
                <h2>{mode === 'create' ? 'Create project' : 'Open project'}</h2>
              </div>
              <button
                type="button"
                className="sf-icon-button sf-close-button"
                onClick={() => setMode(null)}
                disabled={submitting}
                aria-label="Close project dialog"
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </div>
            {mode === 'create' ? (
              <label>
                Project file
                <div className="sf-input-with-button">
                  <span className="sf-selected-file" title={databasePath || undefined}>
                    {databasePath || 'No file selected'}
                  </span>
                  <button
                    type="button"
                    className="sf-button secondary"
                    onClick={async () => {
                      if (!client) return;
                      try {
                        setDatabasePath(forceDuckDbPath(await client.pickDatabaseFile('create')));
                      } catch (error) {
                        notifyApp({
                          kind: 'error',
                          message: error instanceof Error ? error.message : 'File selection failed.',
                        });
                      }
                    }}
                    disabled={submitting}
                  >
                    Create file
                  </button>
                </div>
              </label>
            ) : (
              <label>
                Database path
                <input
                  value={databasePath}
                  onChange={(event) => setDatabasePath(event.target.value)}
                  placeholder="C:/data/project.duckdb"
                  required
                />
              </label>
            )}
            <label>
              Domain
              <select value={domain} onChange={(event) => setDomain(event.target.value)}>
                {domains.map((item) => (
                  <option key={item} value={item}>
                    {item}
                  </option>
                ))}
              </select>
            </label>
            <div className="sf-dialog-actions">
              <button type="button" className="sf-button secondary" onClick={() => setMode(null)} disabled={submitting}>
                Cancel
              </button>
              <button type="submit" className="sf-button" disabled={submitting || (mode === 'create' && !databasePath)}>
                {submitting ? 'Working…' : mode === 'create' ? 'Create project' : 'Open project'}
              </button>
            </div>
          </form>
        </div>
      ) : null}
    </div>
  );
}
function WorkspacePlaceholder({ item }: { item: NavItem }) {
  return (
    <div className="sf-page">
      <div className="sf-page-heading">
        <div>
          <span className="sf-eyebrow">Project workspace</span>
          <h1>{item.label}</h1>
          <p>{item.hint}. This boundary is ready for the backend contract.</p>
        </div>
        <span className="sf-status-pill neutral">No project selected</span>
      </div>
      <div className="sf-workspace-placeholder">
        <div className="sf-placeholder-grid">
          <span />
          <span />
          <span />
        </div>
        <strong>{item.label} is waiting for a project session</strong>
        <p>The frontend shell intentionally contains no hard-coded domain inventory or local database access.</p>
      </div>
    </div>
  );
}
function SettingsPane({
  mode,
  palette,
  style,
  onMode,
  onPalette,
  onStyle,
  onClose,
}: {
  mode: ThemeMode;
  palette: Palette;
  style: Style;
  onMode: (value: ThemeMode) => void;
  onPalette: (value: Palette) => void;
  onStyle: (value: Style) => void;
  onClose: () => void;
}) {
  return (
    <div
      className="sf-side-pane-backdrop sf-settings-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <aside className="sf-side-pane sf-settings-pane" aria-label="Color and style settings">
        <div className="sf-settings-heading">
          <h2>Settings</h2>
          <button className="sf-icon-button sf-close-button" onClick={onClose} aria-label="Close settings">
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
        <section className="sf-settings-section">
          <span className="sf-settings-label">Appearance</span>
          <div className="sf-option-grid">
            {(['light', 'dark'] as ThemeMode[]).map((value) => (
              <button
                key={value}
                className={`sf-option ${mode === value ? 'selected' : ''}`}
                onClick={() => onMode(value)}
              >
                <strong>{value === 'light' ? 'Light' : 'Dark'}</strong>
                <span>{value === 'light' ? 'Bright surface scale' : 'Low-luminance workbench'}</span>
              </button>
            ))}
          </div>
        </section>
        <section className="sf-settings-section">
          <span className="sf-settings-label">Palettes</span>
          <div className="sf-option-grid">
            {palettes.map((item) => (
              <button
                key={item.id}
                className={`sf-option ${palette === item.id ? 'selected' : ''}`}
                onClick={() => onPalette(item.id)}
              >
                <strong>{item.label}</strong>
                <span>{item.description}</span>
                <span className="sf-swatches">
                  {item.swatches.map((swatch) => (
                    <i key={swatch} style={{ background: swatch }} />
                  ))}
                </span>
              </button>
            ))}
          </div>
        </section>
        <section className="sf-settings-section">
          <span className="sf-settings-label">Layout style</span>
          <div className="sf-option-grid">
            {styles.map((item) => (
              <button
                key={item.id}
                className={`sf-option ${style === item.id ? 'selected' : ''}`}
                onClick={() => onStyle(item.id)}
              >
                <strong>{item.label}</strong>
                <span>{item.description}</span>
              </button>
            ))}
          </div>
        </section>
      </aside>
    </div>
  );
}
function BackendStatusPane({
  state,
  onClose,
  endpoint,
}: {
  state: ServiceState;
  onClose: () => void;
  endpoint: string;
}) {
  const details =
    state === 'ready'
      ? 'The app and backend services are available.'
      : state === 'reconnecting'
        ? 'The app is running, but the backend connection is being restored automatically.'
        : state === 'failed'
          ? 'The app is running, but the backend service could not be reached.'
          : 'The app is running while the backend connection is being established.';
  return (
    <div
      className="sf-side-pane-backdrop sf-backend-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <aside className="sf-side-pane sf-backend-pane" aria-label="Application and backend health">
        <div className="sf-backend-heading">
          <div>
            <h2>System health</h2>
            <p className="sf-health-intro">This status checks the app server and its backend connection.</p>
          </div>
          <button className="sf-icon-button sf-close-button" onClick={onClose} aria-label="Close backend details">
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
        <div className="sf-backend-state">
          <span className={`sf-connection-dot ${state}`} />
          <strong>{state === 'ready' ? 'healthy' : state}</strong>
        </div>
        <p>{details}</p>
        <dl>
          <div>
            <dt>App server</dt>
            <dd>
              <span className="sf-health-check">running</span>
            </dd>
          </div>
          <div>
            <dt>Backend server</dt>
            <dd>
              <span className={`sf-health-check ${state}`}>{state === 'ready' ? 'connected' : state}</span>
            </dd>
          </div>
          <div>
            <dt>Endpoint</dt>
            <dd>{endpoint.replace(/^https?:\/\//, '')}</dd>
          </div>
          <div>
            <dt>Protocol</dt>
            <dd>HTTP + WebSocket</dd>
          </div>
          <div>
            <dt>Role</dt>
            <dd>Project and workspace API</dd>
          </div>
        </dl>
      </aside>
    </div>
  );
}
function AppShell({ client, serviceState }: { client: StreamFindApiClient; serviceState: ServiceState }) {
  const [routeState, setRouteState] = useState<ParsedRoute>(routeFromHash);
  const [theme, setTheme] = useState<ThemeMode>(
    () => (localStorage.getItem('streamfind.theme') as ThemeMode) || 'light',
  );
  const [palette, setPalette] = useState<Palette>(
    () => (localStorage.getItem('streamfind.palette') as Palette) || 'streamfind',
  );
  const [style, setStyle] = useState<Style>(() => (localStorage.getItem('streamfind.style') as Style) || 'classic');
  const [collapsed, setCollapsed] = useState(true);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [backendOpen, setBackendOpen] = useState(false);

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      setSettingsOpen(false);
      setBackendOpen(false);
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => window.removeEventListener('keydown', closeOnEscape);
  }, []);
  const [projects, setProjects] = useState<ProjectSession[]>([]);
  const [capabilities, setCapabilities] = useState<ServiceCapabilities>({
    protocol_version: '1.0',
    operations: [],
    endpoints: [],
  });
  const [activeProject, setActiveProject] = useState<ProjectSession | null>(null);
  const [previewProject, setPreviewProject] = useState<ProjectSession | null>(null);
  const projectOpen = activeProject !== null;
  const route = routeState.route;
  const effectiveRoute: RouteKey = projectOpen ? route : 'projects';
  useEffect(() => {
    let active = true;
    const refreshProjects = () =>
      client
        .projects()
        .then((items) => {
          if (active) {
            setProjects(items);
            const sessionId = routeFromHash().sessionId;
            if (sessionId) {
              const project = items.find((item) => item.session_id === sessionId);
              if (project) {
                setActiveProject(project);
              } else {
                setRouteState({ route: 'projects' });
                navigateHash('projects');
              }
            }
          }
        })
        .catch(() => undefined);
    const unsubscribe = client.subscribe((event) => {
      if (event.type === 'project.created' || event.type === 'project.opened' || event.type === 'project.closed')
        refreshProjects();
    });
    client
      .capabilities()
      .then((value) => {
        if (active) setCapabilities(value);
      })
      .catch(() => undefined);
    refreshProjects();
    return () => {
      active = false;
      unsubscribe();
    };
  }, [client]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem('streamfind.theme', theme);
  }, [theme]);
  useEffect(() => {
    document.documentElement.dataset.palette = palette;
    localStorage.setItem('streamfind.palette', palette);
  }, [palette]);
  useEffect(() => {
    document.documentElement.dataset.style = style;
    localStorage.setItem('streamfind.style', style);
  }, [style]);
  useEffect(() => {
    const onHashChange = () => setRouteState(routeFromHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);
  useEffect(() => {
    if (!settingsOpen && !backendOpen && !previewProject) return undefined;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      if (backendOpen) setBackendOpen(false);
      else if (settingsOpen) setSettingsOpen(false);
      else setPreviewProject(null);
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [backendOpen, previewProject, settingsOpen]);
  const visibleNavItems = projectOpen ? navItems : navItems.filter((item) => item.key === 'projects');
  const activeItem = useMemo(
    () => navItems.find((item) => item.key === effectiveRoute) ?? navItems[0],
    [effectiveRoute],
  );
  const navigate = (key: RouteKey) => {
    const sessionId = projectOpen ? activeProject?.session_id : undefined;
    navigateHash(key, sessionId);
    setRouteState({ route: key, sessionId });
  };
  const openProject = (project: ProjectSession) => {
    setPreviewProject(null);
    setActiveProject(project);
    setRouteState({ route: 'workflow', sessionId: project.session_id });
    navigateHash('workflow', project.session_id);
  };

  const closeProject = () => {
    setActiveProject(null);
    setRouteState({ route: 'projects' });
    navigateHash('projects');
  };
  const disconnectProject = async (project: ProjectSession) => {
    try {
      await client.closeProject(project.session_id);
      setProjects((items) => items.filter((item) => item.session_id !== project.session_id));
      if (activeProject?.session_id === project.session_id) closeProject();
      notifyApp({ kind: 'info', message: `Disconnected project ${projectFileName(project)}.` });
    } catch (error) {
      notifyApp({ kind: 'error', message: error instanceof Error ? error.message : 'Project disconnect failed.' });
    }
  };

  return (
    <div className={`sf-shell ${collapsed ? 'collapsed' : ''}`}>
      <aside className="sf-sidebar">
        <div className="sf-brand">
          <img src={logo} alt="" />
          <div>
            <strong>streamfind</strong>
            <span>Scientific workspace</span>
          </div>
        </div>
        <button className="sf-collapse" onClick={() => setCollapsed((value) => !value)} aria-label="Toggle navigation">
          <i className={collapsed ? 'fa-solid fa-angles-right' : 'fa-solid fa-angles-left'} />
        </button>
        <nav>
          {visibleNavItems.map((item) => (
            <button
              key={item.key}
              className={item.key === route ? 'active' : ''}
              onClick={() => navigate(item.key)}
              title={item.hint}
            >
              <span className="sf-nav-icon">
                <i className={item.icon} />
              </span>
              <span>{item.label}</span>
            </button>
          ))}
        </nav>
        <div className="sf-sidebar-footer">
          <span className={`sf-connection-dot ${serviceState}`} />
          <span>Service {serviceState}</span>
        </div>
      </aside>
      <main className="sf-main">
        <header className="sf-topbar">
          <div className="sf-context">
            <img className="sf-topbar-logo" src={logo} alt="" />
            <div className="sf-topbar-brand">
              {activeProject ? (
                <>
                  <strong>{projectFileName(activeProject)}</strong>
                  <span>{activeProject.domain}</span>
                </>
              ) : (
                <>
                  <strong>streamfind</strong>
                  <span>WORKSPACE</span>
                </>
              )}
            </div>
          </div>
          <div className="sf-topbar-actions">
            <button
              className="sf-backend-button"
              onClick={() => setBackendOpen(true)}
              aria-label={`System health: ${serviceState}`}
              title="System health: app and backend"
            >
              <span className={`sf-connection-dot ${serviceState}`} />
              <i className="fa-solid fa-heart-pulse" />
            </button>
            <button
              className="sf-icon-button sf-settings-icon"
              onClick={() => setSettingsOpen(true)}
              aria-label="Open appearance settings"
            >
              <i className="fa-solid fa-gear" />
            </button>
          </div>
        </header>
        <div className="sf-content">
          <ErrorBoundary>
            {activeProject ? (
              <ProjectWorkspace
                project={activeProject}
                capabilities={capabilities}
                client={client}
                onProjectHub={closeProject}
              />
            ) : effectiveRoute === 'projects' ? (
              <ProjectHub
                serviceState={serviceState}
                projects={projects}
                client={client}
                onOpened={openProject}
                onAdded={(project) =>
                  setProjects((items) =>
                    items.some((item) => item.session_id === project.session_id) ? items : [...items, project],
                  )
                }
                onPreview={setPreviewProject}
                onOpenWorkflow={openProject}
                onCloseProject={disconnectProject}
              />
            ) : (
              <WorkspacePlaceholder item={activeItem} />
            )}
          </ErrorBoundary>
        </div>
      </main>
      {previewProject ? (
        <ProjectPreview project={previewProject} onOpenWorkflow={openProject} onClose={() => setPreviewProject(null)} />
      ) : null}
      {settingsOpen ? (
        <SettingsPane
          mode={theme}
          palette={palette}
          style={style}
          onMode={setTheme}
          onPalette={setPalette}
          onStyle={setStyle}
          onClose={() => setSettingsOpen(false)}
        />
      ) : null}
      {backendOpen ? (
        <BackendStatusPane state={serviceState} onClose={() => setBackendOpen(false)} endpoint={client.endpoint()} />
      ) : null}
      <NotificationToasts />
    </div>
  );
}
export default function App() {
  const [client] = useState(() => new StreamFindApiClient());
  const [serviceState, setServiceState] = useState<ServiceState>('disconnected');
  return (
    <BootstrapGate client={client} onState={setServiceState}>
      <AppShell client={client} serviceState={serviceState} />
    </BootstrapGate>
  );
}
