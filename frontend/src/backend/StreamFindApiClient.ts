import type {
  ServiceCapabilities,
  ServiceSession,
  StreamFindEvent,
  ProjectSession,
  WorkflowStateResponse,
  WorkflowValidationResponse,
  JsonValue,
} from './protocol';

export type { ProjectSession, ServiceCapabilities } from './protocol';

export type ServiceState = 'disconnected' | 'connecting' | 'initializing' | 'ready' | 'reconnecting' | 'failed';
export type ServiceControlAction = 'start' | 'stop' | 'restart';
export type SessionResponse = ServiceSession;
export type CapabilitiesResponse = ServiceCapabilities;

type EventHandler = (event: StreamFindEvent) => void;

const defaultBaseUrl = import.meta.env.DEV ? 'http://127.0.0.1:8790' : window.location.origin;

export class StreamFindApiClient {
  private socket: WebSocket | null = null;
  private reconnectTimer: number | null = null;
  private reconnectAttempt = 0;
  private closed = false;
  private readonly eventHandlers = new Set<EventHandler>();

  constructor(private readonly baseUrl = defaultBaseUrl) {}

  endpoint(): string {
    return this.baseUrl;
  }

  async connect(onState: (state: ServiceState) => void): Promise<SessionResponse> {
    this.closed = false;
    onState('connecting');
    try {
      onState('initializing');
      const response = await fetch(`${this.baseUrl}/session`);
      if (!response.ok) throw new Error(`Session request failed (${response.status})`);
      const session = (await response.json()) as SessionResponse;
      await this.capabilities();
      await this.openEvents(onState);
      this.reconnectAttempt = 0;
      onState('ready');
      return session;
    } catch (error) {
      onState('failed');
      this.scheduleReconnect(onState);
      throw error;
    }
  }

  async capabilities(): Promise<CapabilitiesResponse> {
    const response = await fetch(`${this.baseUrl}/capabilities`);
    if (!response.ok) throw new Error(`Capabilities request failed (${response.status})`);
    return response.json() as Promise<CapabilitiesResponse>;
  }

  async projects(): Promise<ProjectSession[]> {
    const response = await fetch(`${this.baseUrl}/projects`);
    if (!response.ok) throw new Error(`Project list request failed (${response.status})`);
    const result = (await response.json()) as { projects: ProjectSession[] };
    return result.projects;
  }

  async closeProject(sessionId: string): Promise<ProjectSession> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}`, { method: 'DELETE' });
    if (!response.ok) throw new Error(`Project close request failed (${response.status})`);
    return response.json() as Promise<ProjectSession>;
  }

  async runOperation(sessionId: string, operationId: string, parameters: Record<string, unknown>): Promise<JsonValue> {
    const response = await fetch(
      `${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/operations/${encodeURIComponent(operationId)}`,
      { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(parameters) },
    );
    if (!response.ok) throw new Error(`Operation request failed (${response.status})`);
    const result = (await response.json()) as { result: JsonValue };
    return result.result;
  }

  async workflowState(sessionId: string): Promise<WorkflowStateResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/state`);
    if (!response.ok) throw new Error(`Workflow state request failed (${response.status})`);
    return response.json() as Promise<WorkflowStateResponse>;
  }

  async validateWorkflow(sessionId: string): Promise<WorkflowValidationResponse> {
    return this.workflowAction<WorkflowValidationResponse>(sessionId, 'validate');
  }

  async runWorkflow(sessionId: string): Promise<WorkflowStateResponse> {
    return this.workflowAction<WorkflowStateResponse>(sessionId, 'run');
  }

  async pauseWorkflow(sessionId: string): Promise<WorkflowStateResponse> {
    return this.workflowAction<WorkflowStateResponse>(sessionId, 'pause');
  }

  async cancelWorkflow(sessionId: string): Promise<WorkflowStateResponse> {
    return this.workflowAction<WorkflowStateResponse>(sessionId, 'cancel');
  }

  private async workflowAction<T extends WorkflowStateResponse>(sessionId: string, action: string): Promise<T> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/${action}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: '{}',
    });
    if (!response.ok) throw new Error(`Workflow ${action} request failed (${response.status})`);
    return response.json() as Promise<T>;
  }

  async controlService(action: ServiceControlAction): Promise<void> {
    const response = await fetch(`http://127.0.0.1:8788/control/${action}`, { method: 'POST' });
    if (!response.ok) throw new Error(`Service ${action} failed (${response.status})`);
  }

  async pickDatabaseFile(mode: 'open' | 'create' = 'open'): Promise<string> {
    const endpoint = mode === 'create' ? '/projects/file-picker/create' : '/projects/file-picker';
    const response = await fetch(`${this.baseUrl}${endpoint}`);
    const result = (await response.json()) as { database_path?: string; error?: string };
    if (!response.ok || !result.database_path)
      throw new Error(result.error || `Database file selection failed (${response.status})`);
    return result.database_path;
  }

  async pickFiles(extensions: string[]): Promise<string[]> {
    const response = await fetch(`${this.baseUrl}/projects/file-picker/files`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ extensions }),
    });
    const result = (await response.json()) as { paths?: string[]; error?: string };
    if (!response.ok || !result.paths) throw new Error(result.error || `File selection failed (${response.status})`);
    return result.paths;
  }

  async pickFolders(): Promise<string[]> {
    const response = await fetch(`${this.baseUrl}/projects/file-picker/folders`, { method: 'POST' });
    const result = (await response.json()) as { paths?: string[]; error?: string };
    if (!response.ok || !result.paths) throw new Error(result.error || `Folder selection failed (${response.status})`);
    return result.paths;
  }

  async pickPaths(extensions: string[]): Promise<string[]> {
    const response = await fetch(`${this.baseUrl}/projects/file-picker/paths`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ extensions }),
    });
    const result = (await response.json()) as { paths?: string[]; error?: string };
    if (!response.ok || !result.paths) throw new Error(result.error || `Path selection failed (${response.status})`);
    return result.paths;
  }

  async createProject(input: {
    session_id: string;
    database_path: string;
    domain: string;
    metadata?: Record<string, unknown>;
    mode?: 'create' | 'open';
  }): Promise<ProjectSession> {
    const response = await fetch(`${this.baseUrl}/projects`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(input),
    });
    const result = (await response.json()) as ProjectSession & { error?: string };
    if (!response.ok) throw new Error(result.error || `Project request failed (${response.status})`);
    return result;
  }

  subscribe(handler: EventHandler): () => void {
    this.eventHandlers.add(handler);
    return () => this.eventHandlers.delete(handler);
  }
  disconnect(): void {
    this.closed = true;
    if (this.reconnectTimer !== null) window.clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.socket?.close();
    this.socket = null;
  }

  private scheduleReconnect(onState: (state: ServiceState) => void): void {
    if (this.closed || this.reconnectTimer !== null) return;
    const delay = Math.min(30_000, 1_000 * 2 ** this.reconnectAttempt++);
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null;
      this.connect(onState).catch(() => undefined);
    }, delay);
  }

  private openEvents(onState: (state: ServiceState) => void): Promise<void> {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(this.baseUrl.replace(/^http/, 'ws') + '/events');
      this.socket = socket;
      socket.onopen = () => resolve();
      socket.onerror = () => reject(new Error('Event channel connection failed'));
      socket.onclose = () => {
        this.socket = null;
        if (!this.closed) {
          onState('reconnecting');
          this.scheduleReconnect(onState);
        }
      };
      socket.onmessage = (message) => {
        try {
          this.eventHandlers.forEach((handler) => handler(JSON.parse(message.data) as StreamFindEvent));
        } catch {
          /* Ignore malformed event frames. */
        }
      };
    });
  }
}
