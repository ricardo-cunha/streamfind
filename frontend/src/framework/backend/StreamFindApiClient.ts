import type {
  ServiceCapabilities,
  ServiceSession,
  StreamFindEvent,
  ProjectSession,
  WorkflowStateResponse,
  WorkflowDefinition,
  WorkflowDefinitionResponse,
  WorkflowEventHistoryResponse,
  JsonValue,
  CapabilityIndexResponse,
  CapabilityModulesResponse,
  CapabilityOperationSummary,
  CapabilityOperationsRequest,
  BackendCapability,
  DependencyDescriptor,
  DependencyInstallResult,
  WorkflowDemoMetadata,
} from './protocol';

export type { ProjectSession, ServiceCapabilities } from './protocol';

export type ServiceState = 'disconnected' | 'connecting' | 'initializing' | 'ready' | 'reconnecting' | 'failed';
export type ServiceControlAction = 'start' | 'stop' | 'restart';
export type SessionResponse = ServiceSession;
export type CapabilitiesResponse = ServiceCapabilities;
export type FileSystemEntry = {
  name: string;
  path: string;
  isDirectory: boolean;
  size?: number;
};
export type ArtifactRecord = {
  artifact_id: string;
  contract_id: string;
  representation: 'json' | 'table' | string;
  physical_table?: string | null;
  payload?: JsonValue | null;
  row_count?: number;
  columns?: Array<{ name: string; type: string }>;
  producer_operation: string;
  producer_instance: string;
  workflow_revision: number;
  status: string;
  created_at?: string;
};
export type ArtifactDataRequest = {
  artifact_id: string;
  offset?: number;
  limit?: number;
  search?: string;
  sort_column?: string;
  descending?: boolean;
};
export type ArtifactDataResponse = {
  artifact_id: string;
  columns: Array<{ name: string; type: string }>;
  rows: Array<Record<string, string | null>>;
  offset: number;
  limit: number;
  total_rows: number;
};
export type ArtifactQueryFilter = {
  column: string;
  operator: 'eq' | 'neq' | 'gt' | 'gte' | 'lt' | 'lte' | 'between' | 'in' | 'is_null' | 'is_not_null';
  value?: JsonValue;
  values?: JsonValue[];
  min?: JsonValue;
  max?: JsonValue;
};
export type ArtifactQueryRequest = {
  artifact_id: string;
  mode?: 'page' | 'sample' | 'detail';
  columns?: string[];
  filters?: ArtifactQueryFilter[];
  offset?: number;
  limit?: number;
  search?: string;
  sort_column?: string;
  descending?: boolean;
  x_column?: string;
  y_column?: string;
  row_keys?: string[];
  sampling_strategy?: 'ordered' | 'grid';
  x_bins?: number;
  y_bins?: number;
};
export type ArtifactQueryResponse = ArtifactDataResponse & {
  returned_rows: number;
  mode: 'page' | 'sample' | 'detail';
  has_more: boolean;
};
export type StructureSvgRequest = {
  smiles?: string | null;
  inchi?: string | null;
  width?: number;
  height?: number;
  bond_color?: string;
};
export type IsotopePatternRequest = {
  formula: string;
  charge?: number;
  probability?: number;
  max_peaks?: number;
};
export type IsotopePatternResponse = {
  formula: string;
  mz: number[];
  probability: number[];
  labels: string[];
};

type EventHandler = (event: StreamFindEvent) => void;

const defaultBaseUrl = import.meta.env.DEV ? 'http://127.0.0.1:8790' : window.location.origin;

export class StreamFindApiClient {
  private socket: WebSocket | null = null;
  private reconnectTimer: number | null = null;
  private reconnectAttempt = 0;
  private closed = false;
  private connectionGeneration = 0;
  private readonly eventHandlers = new Set<EventHandler>();

  constructor(private readonly baseUrl = defaultBaseUrl) {}

  endpoint(): string {
    return this.baseUrl;
  }

  async connect(onState: (state: ServiceState) => void): Promise<SessionResponse> {
    this.closed = false;
    const generation = ++this.connectionGeneration;
    onState('connecting');
    try {
      onState('initializing');
      const response = await fetch(`${this.baseUrl}/session`);
      if (generation !== this.connectionGeneration) throw new Error('stale service connection attempt');
      if (!response.ok) throw new Error(`Session request failed (${response.status})`);
      const session = (await response.json()) as SessionResponse;
      await this.capabilitiesIndex();
      await this.openEvents(onState, generation);
      this.reconnectAttempt = 0;
      onState('ready');
      return session;
    } catch (error) {
      if (generation !== this.connectionGeneration) throw error;
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

  async dependencies(): Promise<DependencyDescriptor[]> {
    const response = await fetch(`${this.baseUrl}/dependencies`);
    if (!response.ok) throw new Error(`Dependency discovery request failed (${response.status})`);
    const result = (await response.json()) as { dependencies?: DependencyDescriptor[] };
    return result.dependencies || [];
  }

  async installDependencies(dependencyIds: string[], allowNetwork = false): Promise<DependencyInstallResult[]> {
    const response = await fetch(`${this.baseUrl}/dependencies/install`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ dependency_ids: dependencyIds, allow_network: allowNetwork }),
    });
    const result = (await response.json()) as { results?: DependencyInstallResult[]; error?: string };
    if (!response.ok) throw new Error(result.error || `Dependency installation request failed (${response.status})`);
    return result.results || [];
  }

  async capabilitiesIndex(): Promise<CapabilityIndexResponse> {
    const response = await fetch(`${this.baseUrl}/capabilities/index`);
    if (!response.ok) throw new Error(`Capabilities index request failed (${response.status})`);
    return response.json() as Promise<CapabilityIndexResponse>;
  }

  async capabilityModules(domain: string): Promise<CapabilityModulesResponse> {
    const response = await fetch(`${this.baseUrl}/capabilities/domains/${encodeURIComponent(domain)}/modules`);
    if (!response.ok) throw new Error(`Capability module request failed (${response.status})`);
    return response.json() as Promise<CapabilityModulesResponse>;
  }

  async capabilityOperations(
    options: CapabilityOperationsRequest & { includeSchema: true },
  ): Promise<BackendCapability[]>;
  async capabilityOperations(
    options: CapabilityOperationsRequest & { includeSchema?: false },
  ): Promise<CapabilityOperationSummary[]>;
  async capabilityOperations(
    options: CapabilityOperationsRequest,
  ): Promise<CapabilityOperationSummary[] | BackendCapability[]> {
    const query = new URLSearchParams({ domain: options.domain });
    if (options.module) query.set('module', options.module);
    if (options.search) query.set('search', options.search);
    if (options.includeSchema) query.set('include_schema', 'true');
    const response = await fetch(`${this.baseUrl}/capabilities/operations?${query.toString()}`);
    if (!response.ok) throw new Error(`Capability operations request failed (${response.status})`);
    const result = (await response.json()) as { operations: CapabilityOperationSummary[] | BackendCapability[] };
    return result.operations;
  }

  async capabilityOperation(canonicalId: string): Promise<BackendCapability> {
    const response = await fetch(`${this.baseUrl}/capabilities/operations/${encodeURIComponent(canonicalId)}`);
    if (!response.ok) throw new Error(`Capability operation request failed (${response.status})`);
    return response.json() as Promise<BackendCapability>;
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

  async runOperation(
    sessionId: string,
    operationId: string,
    parameters: Record<string, unknown>,
    operationInstance?: string,
  ): Promise<JsonValue> {
    const response = await fetch(
      `${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/operations/${encodeURIComponent(operationId)}`,
      {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ parameters, operation_instance: operationInstance || operationId }),
      },
    );
    if (!response.ok) {
      let detail = '';
      try {
        const payload = (await response.json()) as { error?: string };
        detail = payload.error ? `: ${payload.error}` : '';
      } catch {
        // Preserve the HTTP status when the service returned no JSON body.
      }
      throw new Error(`Operation request failed (${response.status})${detail}`);
    }
    const result = (await response.json()) as { result: JsonValue };
    return result.result;
  }

  async clearWorkflowHistory(sessionId: string): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/history/clear`, {
      method: 'POST',
    });
    const result = (await response.json()) as WorkflowDefinitionResponse & { error?: string };
    if (!response.ok) throw new Error(result.error || `Workflow history clear request failed (${response.status})`);
    return result;
  }

  async clearArtifactCache(sessionId: string): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/cache/clear`, {
      method: 'POST',
    });
    const result = (await response.json()) as WorkflowDefinitionResponse & { error?: string };
    if (!response.ok) throw new Error(result.error || `Artifact cache clear request failed (${response.status})`);
    return result;
  }

  async clearAllArtifacts(sessionId: string): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/clear`, {
      method: 'POST',
    });
    const result = (await response.json()) as WorkflowDefinitionResponse & { error?: string };
    if (!response.ok) throw new Error(result.error || `Artifact clear request failed (${response.status})`);
    return result;
  }

  async workflowState(sessionId: string): Promise<WorkflowStateResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/state`);
    if (!response.ok) throw new Error(`Workflow state request failed (${response.status})`);
    return response.json() as Promise<WorkflowStateResponse>;
  }

  async workflowEvents(sessionId: string, after = 0, limit = 20000): Promise<WorkflowEventHistoryResponse> {
    const response = await fetch(
      `${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/events?after=${after}&limit=${limit}`,
    );
    if (!response.ok) throw new Error(`Workflow event history request failed (${response.status})`);
    return response.json() as Promise<WorkflowEventHistoryResponse>;
  }

  async artifacts(sessionId: string): Promise<ArtifactRecord[]> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts`);
    if (!response.ok) throw new Error(`Artifact inventory request failed (${response.status})`);
    const result = (await response.json()) as { artifacts?: ArtifactRecord[] };
    return result.artifacts || [];
  }

  async currentArtifacts(sessionId: string): Promise<ArtifactRecord[]> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/current`);
    if (!response.ok) throw new Error(`Current artifact inventory request failed (${response.status})`);
    const result = (await response.json()) as { artifacts?: ArtifactRecord[] };
    return result.artifacts || [];
  }

  async clearNodeArtifacts(sessionId: string, producerInstance: string): Promise<void> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/node/clear`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ producer_instance: producerInstance }),
    });
    if (!response.ok) throw new Error(`Node artifact cleanup failed (${response.status})`);
  }

  async artifactData(sessionId: string, request: ArtifactDataRequest): Promise<ArtifactDataResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/data`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
    if (!response.ok) throw new Error(`Artifact data request failed (${response.status})`);
    return (await response.json()) as ArtifactDataResponse;
  }

  async artifactQuery(sessionId: string, request: ArtifactQueryRequest): Promise<ArtifactQueryResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/artifacts/query`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
    if (!response.ok) throw new Error(`Artifact query request failed (${response.status})`);
    return (await response.json()) as ArtifactQueryResponse;
  }

  async structureSvg(request: StructureSvgRequest): Promise<string> {
    const response = await fetch(`${this.baseUrl}/chemistry/structure-svg`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
    const result = (await response.json()) as { svg?: string; error?: string };
    if (!response.ok || !result.svg)
      throw new Error(result.error || `Structure rendering request failed (${response.status})`);
    return result.svg;
  }

  async isotopePattern(request: IsotopePatternRequest): Promise<IsotopePatternResponse> {
    const response = await fetch(`${this.baseUrl}/chemistry/isotope-pattern`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(request),
    });
    const result = (await response.json()) as IsotopePatternResponse & { error?: string };
    if (
      !response.ok ||
      !Array.isArray(result.mz) ||
      !Array.isArray(result.probability) ||
      !Array.isArray(result.labels)
    )
      throw new Error(result.error || `Isotope pattern request failed (${response.status})`);
    return result;
  }

  async workflowDefinition(sessionId: string): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow`);
    if (!response.ok) throw new Error(`Workflow definition request failed (${response.status})`);
    return response.json() as Promise<WorkflowDefinitionResponse>;
  }

  async workflowDemos(): Promise<WorkflowDemoMetadata[]> {
    const response = await fetch(`${this.baseUrl}/workflow-demos`);
    if (!response.ok) throw new Error(`Workflow demo list request failed (${response.status})`);
    const result = (await response.json()) as { workflows?: WorkflowDemoMetadata[] };
    return result.workflows || [];
  }

  async validateWorkflow(sessionId: string, workflow: WorkflowDefinition): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow/validate`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(workflow),
    });
    if (!response.ok) throw new Error(`Workflow validation request failed (${response.status})`);
    return response.json() as Promise<WorkflowDefinitionResponse>;
  }

  async saveWorkflow(sessionId: string, workflow: WorkflowDefinition): Promise<WorkflowDefinitionResponse> {
    const response = await fetch(`${this.baseUrl}/projects/${encodeURIComponent(sessionId)}/workflow`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(workflow),
    });
    const result = (await response.json()) as WorkflowDefinitionResponse & { error?: string };
    if (!response.ok) throw new Error(result.error || `Workflow save request failed (${response.status})`);
    return result;
  }

  async runWorkflow(sessionId: string): Promise<WorkflowStateResponse> {
    return this.workflowAction<WorkflowStateResponse>(sessionId, 'run');
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

  async browseFileSystem(path = ''): Promise<{ path: string; entries: FileSystemEntry[] }> {
    const response = await fetch(`${this.baseUrl}/projects/file-picker/browse`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ path }),
    });
    const result = (await response.json()) as { path?: string; entries?: FileSystemEntry[]; error?: string };
    if (!response.ok || typeof result.path !== 'string' || !result.entries)
      throw new Error(result.error || `Filesystem browse failed (${response.status})`);
    return { path: result.path, entries: result.entries };
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
    metadata?: Record<string, unknown>;
    workflow_metadata?: Record<string, unknown>;
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
    this.connectionGeneration += 1;
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

  private openEvents(onState: (state: ServiceState) => void, generation: number): Promise<void> {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(this.baseUrl.replace(/^http/, 'ws') + '/events');
      this.socket = socket;
      socket.onopen = () => {
        if (generation !== this.connectionGeneration) {
          socket.close();
          reject(new Error('stale event connection attempt'));
          return;
        }
        resolve();
      };
      socket.onerror = () => reject(new Error('Event channel connection failed'));
      socket.onclose = () => {
        if (this.socket === socket) this.socket = null;
        if (!this.closed && generation === this.connectionGeneration) {
          onState('reconnecting');
          this.scheduleReconnect(onState);
        }
      };
      socket.onmessage = (message) => {
        if (generation !== this.connectionGeneration) return;
        try {
          this.eventHandlers.forEach((handler) => handler(JSON.parse(message.data) as StreamFindEvent));
        } catch {
          /* Ignore malformed event frames. */
        }
      };
    });
  }
}
