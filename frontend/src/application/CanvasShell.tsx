import {
  Fragment,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  useCallback,
  type ChangeEvent,
  type MouseEvent as ReactMouseEvent,
} from 'react';
import {
  StreamFindApiClient,
  type ArtifactRecord,
  type ProjectSession,
} from '../framework/backend/StreamFindApiClient';
import type { StreamFindEvent, WorkflowMetadata } from '../framework/backend/protocol';
import { LoadingLogo } from '../framework/ui/LoadingLogo';

import { PathFileManager } from './PathFileManager';
import { subscribeAppNotifications } from '../framework/notifications/notificationBus';
import { ArtifactViewerHost } from '../framework/viewers/ArtifactViewerHost';
import { compatibleArtifactViewers } from '../framework/viewers/viewerTypes';

import { frontendPluginApi } from '../framework/plugins/FrontendPluginRegistry';
import {
  canvasWorkflow,
  canvasSnapshotFingerprint,
  nextCanvasNodeNumber,
  redoCanvasHistory,
  recordCanvasHistory,
  type CanvasHistoryState,
  undoCanvasHistory,
  workflowFingerprint,
  workflowLayout,
  workflowToCanvas,
  type CanvasEdge,
  type CanvasHistorySnapshot,
  type CanvasLogLine,
  type CanvasNode,
  type ConnectionDraft,
  type Interaction,
  type Point,
} from './workflow/workflowModel';
import {
  defaultParameters,
  fileParameterExtensions,
  isFileListParameter,
  isJsonParameter,
  isPathListParameter,
  isSimplePathParameter,
  isTableParameter,
  normalizeInlineTableRows,
  parameterInputValue,
  parseCsvRows,
  scalarInputValue,
  schemaTypeLabel,
  schemaPropertyOrder,
  uiParameters,
  validateInlineTableRows,
  validateJsonValue,
  wireParameters,
} from './workflow/parameterModel';
import {
  nodePorts,
  parameterTypeKey,
  portMatchesParameter,
  portMatchesPort,
  typeClass,
  typeIcon,
  visualPortTypeKey,
  type NodePort,
} from './workflow/portModel';
import { edgeTypeKey, portPosition } from './workflow/portGeometry';
import {
  CANVAS_WORLD_HEIGHT,
  CANVAS_WORLD_WIDTH,
  connectedNodePosition,
  GRID_SIZE,
  NODE_HEIGHT,
  NODE_WIDTH,
} from './workflow/canvasGeometry';
import {
  capabilityTemplates,
  operationDeckDensity,
  type DocumentationFocus,
  type NodeTemplate,
} from './workflow/capabilityModel';
import { ontologyPortTerm, parameterOntologyDetails, portOntologyDetails } from './workflow/ontologyModel';
import {
  artifactContractMatches,
  artifactSummary,
  isVisualizationArtifact,
  prettyArtifactPayload,
} from './results/artifactDisplay';
import type {
  BackendCapability,
  CapabilityParameter,
  JsonSchema,
  ServiceCapabilities,
  WorkflowDefinition,
  WorkflowState,
} from '../framework/backend/protocol';

export default function CanvasShell({
  project,
  capabilities,
  surface,
  client,
  onProjectHub,
  onOpenOntologyWiki,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  surface: 'workflow' | 'explorer';
  client?: StreamFindApiClient;
  onProjectHub?: () => void;
  onOpenOntologyWiki?: (term?: string) => void;
}) {
  const pluginApi = useMemo(() => (client ? frontendPluginApi(client) : undefined), [client]);
  const canvasRef = useRef<HTMLDivElement | null>(null);
  const workflowFileInputRef = useRef<HTMLInputElement | null>(null);
  const nextId = useRef(1);
  const [nodes, setNodes] = useState<CanvasNode[]>([]);
  const nodesRef = useRef<CanvasNode[]>([]);
  const [edges, setEdges] = useState<CanvasEdge[]>([]);
  const [scale, setScale] = useState(1);
  const [offset, setOffset] = useState<Point>({ x: 0, y: 0 });
  const initialViewportFittedRef = useRef(false);
  const [worldSize, setWorldSize] = useState({ width: CANVAS_WORLD_WIDTH, height: CANVAS_WORLD_HEIGHT });
  const worldSizeRef = useRef({ width: CANVAS_WORLD_WIDTH, height: CANVAS_WORLD_HEIGHT });
  const viewportRef = useRef({ offset, scale });
  useEffect(() => {
    viewportRef.current = { offset, scale };
  }, [offset, scale]);
  useEffect(() => {
    worldSizeRef.current = worldSize;
  }, [worldSize]);
  const canvasWorldSize = useMemo(() => {
    const maximumX = Math.max(NODE_WIDTH, ...nodes.map((node) => node.x + NODE_WIDTH));
    const maximumY = Math.max(NODE_HEIGHT, ...nodes.map((node) => node.y + NODE_HEIGHT));
    return {
      width: Math.max(CANVAS_WORLD_WIDTH, worldSize.width, maximumX + 800),
      height: Math.max(CANVAS_WORLD_HEIGHT, worldSize.height, maximumY + 600),
    };
  }, [nodes, worldSize]);
  const [interaction, setInteraction] = useState<Interaction | null>(null);
  const [selectedNodeIds, setSelectedNodeIds] = useState<string[]>([]);
  const [selectionBox, setSelectionBox] = useState<{ x: number; y: number; width: number; height: number } | null>(
    null,
  );
  const rightDragRef = useRef<{ start: Point; moved: boolean } | null>(null);
  const suppressContextMenuRef = useRef(false);
  const dragPointerRef = useRef<{ clientX: number; clientY: number } | null>(null);
  const dragFrameRef = useRef<number | null>(null);
  const dragExpandedEdgesRef = useRef({ left: false, right: false, top: false, bottom: false });
  const [connection, setConnection] = useState<ConnectionDraft | null>(null);
  const pendingConnectionRef = useRef<{
    source: string;
    sourcePort: string;
    startX: number;
    startY: number;
  } | null>(null);
  const [picker, setPicker] = useState<(Point & { source?: string; sourcePort?: string }) | null>(null);
  const [pickerCapabilityId, setPickerCapabilityId] = useState<string | null>(null);
  const [jsonEditor, setJsonEditor] = useState<{ nodeId: string; parameter: CapabilityParameter } | null>(null);
  const [pathWizard, setPathWizard] = useState<{
    nodeId: string;
    parameter: CapabilityParameter;
    mergeIntoJsonEditor?: boolean;
  } | null>(null);

  const [tableEditor, setTableEditor] = useState<{
    nodeId: string;
    parameter: CapabilityParameter;
    rows: Record<string, unknown>[];
    returnToJsonEditor?: boolean;
    error?: string | null;
  } | null>(null);
  const [csvPreview, setCsvPreview] = useState<{ rows: Record<string, unknown>[]; error?: string } | null>(null);
  const [jsonEditorText, setJsonEditorText] = useState('');
  const [jsonEditorError, setJsonEditorError] = useState<string | null>(null);
  const [workflowMetadata, setWorkflowMetadata] = useState<WorkflowMetadata>({});
  const [metadataEditorOpen, setMetadataEditorOpen] = useState(false);
  const [metadataEditorText, setMetadataEditorText] = useState('');
  const [metadataEditorError, setMetadataEditorError] = useState<string | null>(null);
  const [gridVisible, setGridVisible] = useState(false);
  const [workflowState, setWorkflowState] = useState<WorkflowState>('idle');
  const [, setWorkflowProgress] = useState({ completed: 0, total: 0, current_step: 0 });
  const workflowExecutionIdRef = useRef<string | null>(null);
  const [workflowBusy, setWorkflowBusy] = useState(false);
  const [savingWorkflow, setSavingWorkflow] = useState(false);
  const [artifactCleanupNode, setArtifactCleanupNode] = useState<CanvasNode | null>(null);
  const [artifactCleanupBusy, setArtifactCleanupBusy] = useState(false);
  const [workflowRevision, setWorkflowRevision] = useState(1);
  const [workflowLoaded, setWorkflowLoaded] = useState(surface !== 'workflow' || !client);
  const [workflowReloadNonce, setWorkflowReloadNonce] = useState(0);
  const [savedWorkflowFingerprint, setSavedWorkflowFingerprint] = useState<string | null>(null);
  const lastObservedWorkflowStateRef = useRef<WorkflowState | null>(null);
  const artifactsLoadedRef = useRef(false);
  const currentArtifactsRef = useRef<ArtifactRecord[]>([]);
  const artifactsRefreshInFlightRef = useRef<Promise<ArtifactRecord[]> | null>(null);

  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [documentationFocus, setDocumentationFocus] = useState<DocumentationFocus | null>(null);
  const [selectedEdgeId, setSelectedEdgeId] = useState<string | null>(null);
  const lastStatusRef = useRef<{ message: string; timestamp: number } | null>(null);
  const recentEventRef = useRef<Map<string, number>>(new Map());
  const seenEventIdsRef = useRef<Set<number>>(new Set());
  const [paletteSearch, setPaletteSearch] = useState('');
  const [paletteModule, setPaletteModule] = useState('');
  const [anchorCenters, setAnchorCenters] = useState<Record<string, Point>>({});
  const anchorCentersRef = useRef<Record<string, Point>>({});
  const [expandedParameters, setExpandedParameters] = useState<Record<string, boolean>>({});
  const [status, setStatusState] = useState(
    surface === 'workflow' ? 'Drag an output connector to a compatible input connector.' : 'Workflow canvas',
  );
  const [activityLog, setActivityLog] = useState<CanvasLogLine[]>([]);
  const logSequenceRef = useRef(0);
  const [currentArtifacts, setCurrentArtifacts] = useState<ArtifactRecord[]>([]);
  const [artifactViewer, setArtifactViewer] = useState<ArtifactRecord | null>(null);
  const [artifactViewerMode, setArtifactViewerMode] = useState<'table' | 'default'>('default');
  const [artifactViewerId, setArtifactViewerId] = useState<string | null>(null);
  const [hoveredOutputAnchor, setHoveredOutputAnchor] = useState<string | null>(null);
  const outputHoverTimeoutRef = useRef<number | null>(null);

  useEffect(() => {
    nodesRef.current = nodes;
  }, [nodes]);

  const openArtifactViewer = (artifact: ArtifactRecord, mode: 'table' | 'default', viewerId: string | null = null) => {
    setArtifactViewer(artifact);
    setArtifactViewerMode(mode);
    setArtifactViewerId(viewerId);
  };
  const openDefaultArtifactViewer = (artifact: ArtifactRecord) => {
    const defaultViewer = compatibleArtifactViewers({
      semanticType: artifact.contract_id,
      representation: artifact.representation,
    })[0];
    openArtifactViewer(artifact, 'default', defaultViewer?.id ?? null);
  };
  const showOutputRendererMenu = (key: string) => {
    if (outputHoverTimeoutRef.current !== null) window.clearTimeout(outputHoverTimeoutRef.current);
    setHoveredOutputAnchor(key);
    if (!artifactsLoadedRef.current) void refreshArtifacts();
  };
  const hideOutputRendererMenu = () => {
    if (outputHoverTimeoutRef.current !== null) window.clearTimeout(outputHoverTimeoutRef.current);
    outputHoverTimeoutRef.current = window.setTimeout(() => setHoveredOutputAnchor(null), 300);
  };
  const historyRef = useRef<CanvasHistoryState>({
    past: [],
    future: [],
    current: '',
  });
  const historyApplyingRef = useRef(false);
  const appendLog = useCallback((message: string, level: CanvasLogLine['level'] = 'info', timestampMs?: number) => {
    const timestamp = timestampMs ?? Date.now();
    setActivityLog((current) => [
      {
        id: ++logSequenceRef.current,
        timestamp: new Date(timestamp).toLocaleTimeString(),
        message,
        level,
      },
      ...current.slice(0, 4999),
    ]);
  }, []);
  const resetActivityLog = useCallback(() => {
    logSequenceRef.current = 0;
    setActivityLog([]);
  }, []);
  useEffect(() => {
    return subscribeAppNotifications((notification) => {
      const level: CanvasLogLine['level'] =
        notification.kind === 'error' ? 'error' : notification.kind === 'success' ? 'success' : 'info';
      appendLog(`notification: ${notification.message}`, level);
    });
  }, [appendLog]);
  const setStatus = useCallback(
    (message: string) => {
      const previous = lastStatusRef.current;
      const now = Date.now();
      if (previous?.message === message && now - previous.timestamp < 1000) return;
      lastStatusRef.current = { message, timestamp: now };
      setStatusState(message);
      appendLog(
        message,
        message.toLowerCase().includes('failed') || message.toLowerCase().includes('error') ? 'error' : 'info',
      );
    },
    [appendLog],
  );
  const refreshArtifacts = useCallback(
    (force = false) => {
      if (!client || typeof client.currentArtifacts !== 'function') return Promise.resolve<ArtifactRecord[]>([]);
      if (!force && artifactsLoadedRef.current) return Promise.resolve(currentArtifactsRef.current);
      if (artifactsRefreshInFlightRef.current) return artifactsRefreshInFlightRef.current;
      const request = client
        .currentArtifacts(project.session_id)
        .then((artifacts) => {
          setCurrentArtifacts(artifacts);
          currentArtifactsRef.current = artifacts;
          artifactsLoadedRef.current = true;
          return artifacts;
        })
        .catch(() => []);
      artifactsRefreshInFlightRef.current = request;
      void request.finally(() => {
        artifactsRefreshInFlightRef.current = null;
      });
      return request;
    },
    [client, project.session_id],
  );

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      if (artifactViewer) {
        setArtifactViewer(null);
        return;
      }
      if (jsonEditor || pathWizard || tableEditor) {
        setJsonEditor(null);
        setPathWizard(null);
        setTableEditor(null);
        return;
      }
      if (artifactCleanupNode && !artifactCleanupBusy) {
        setArtifactCleanupNode(null);
        return;
      }
      if (selectedNodeId || pickerCapabilityId) {
        setSelectedNodeId(null);
        setPickerCapabilityId(null);
        return;
      }
      if (picker) {
        setPicker(null);
        return;
      }
      setConnection(null);
      pendingConnectionRef.current = null;
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => window.removeEventListener('keydown', closeOnEscape);
  }, [
    artifactCleanupBusy,
    artifactCleanupNode,
    artifactViewer,
    jsonEditor,
    pathWizard,
    picker,
    pickerCapabilityId,
    selectedNodeId,
    tableEditor,
  ]);
  const nodeTemplates = useMemo(() => capabilityTemplates(capabilities), [capabilities]);
  const currentWorkflow = useMemo(
    () => canvasWorkflow(nodes, edges, capabilities, workflowRevision, workflowMetadata),
    [capabilities, edges, nodes, workflowMetadata, workflowRevision],
  );
  const currentWorkflowFingerprint = useMemo(() => workflowFingerprint(currentWorkflow), [currentWorkflow]);

  useEffect(() => {
    if (!workflowLoaded) return;
    if (interaction?.type === 'node' || interaction?.type === 'marquee') return;
    const snapshot = { nodes, edges };
    const fingerprint = canvasSnapshotFingerprint(snapshot);
    if (historyApplyingRef.current) {
      historyApplyingRef.current = false;
      historyRef.current.current = fingerprint;
      return;
    }
    if (!historyRef.current.current) {
      historyRef.current.current = fingerprint;
      return;
    }
    if (historyRef.current.current === fingerprint) return;
    historyRef.current = recordCanvasHistory(historyRef.current, snapshot);
  }, [edges, interaction, nodes, workflowLoaded]);

  const restoreHistorySnapshot = (snapshot: CanvasHistorySnapshot) => {
    historyApplyingRef.current = true;
    setNodes(snapshot.nodes);
    setEdges(snapshot.edges);
  };

  const undoWorkflowEdit = () => {
    const history = historyRef.current;
    const result = undoCanvasHistory(history);
    if (!result) return;
    historyRef.current = result.history;
    restoreHistorySnapshot(result.snapshot);
    setStatus('Undid the last workflow edit. Save it to persist the change.');
  };

  const redoWorkflowEdit = () => {
    const history = historyRef.current;
    const result = redoCanvasHistory(history);
    if (!result) return;
    historyRef.current = result.history;
    restoreHistorySnapshot(result.snapshot);
    setStatus('Redid the workflow edit. Save it to persist the change.');
  };

  const workflowDirty =
    workflowLoaded && savedWorkflowFingerprint !== null && currentWorkflowFingerprint !== savedWorkflowFingerprint;

  const applyWorkflowEvent = useCallback(
    (event: StreamFindEvent) => {
      const projectId =
        typeof event.project === 'string'
          ? event.project
          : event.project &&
              typeof event.project === 'object' &&
              !Array.isArray(event.project) &&
              'session_id' in event.project
            ? String(event.project.session_id)
            : undefined;
      if (projectId && projectId !== project.session_id) return;
      if (
        event.type !== 'workflow.started' &&
        event.execution_id &&
        workflowExecutionIdRef.current &&
        event.execution_id !== workflowExecutionIdRef.current
      )
        return;
      if (event.event_id !== undefined) {
        if (seenEventIdsRef.current.has(event.event_id)) return;
        seenEventIdsRef.current.add(event.event_id);
        if (seenEventIdsRef.current.size > 50000) {
          const first = seenEventIdsRef.current.values().next().value;
          if (first !== undefined) seenEventIdsRef.current.delete(first);
        }
      }
      const payload = event.payload;
      const detail =
        payload && typeof payload === 'object' && !Array.isArray(payload) && 'message' in payload
          ? String(payload.message)
          : undefined;
      const operation = event.operation_id ? ` (${event.operation_id})` : '';
      const payloadLevel =
        payload && typeof payload === 'object' && !Array.isArray(payload) && 'level' in payload
          ? String(payload.level)
          : undefined;
      const level: CanvasLogLine['level'] =
        payloadLevel === 'error' || event.type.endsWith('.failed')
          ? 'error'
          : payloadLevel === 'warning'
            ? 'info'
            : event.type.endsWith('.completed')
              ? 'success'
              : 'info';
      if (event.operation_id) {
        const executionState =
          event.type === 'operation.started'
            ? 'running'
            : event.type === 'operation.completed'
              ? 'completed'
              : event.type === 'operation.failed'
                ? 'failed'
                : undefined;
        if (executionState) {
          setNodes((current) =>
            current.map((node) => (node.id === event.operation_id ? { ...node, executionState } : node)),
          );
        }
      }
      if (event.type === 'workflow.started') {
        workflowExecutionIdRef.current = event.execution_id ?? workflowExecutionIdRef.current;
        resetActivityLog();
        setWorkflowState('queued');
      }
      if (event.type !== 'workflow.completed') {
        appendLog(
          detail ? `${event.type}${operation}: ${detail}` : `${event.type}${operation}`,
          level,
          event.timestamp_ms,
        );
      }
      if (
        event.type === 'workflow.completed' ||
        event.type === 'workflow.failed' ||
        event.type === 'workflow.cancelled'
      ) {
        const terminalState =
          event.type === 'workflow.completed' ? 'completed' : event.type === 'workflow.failed' ? 'failed' : 'cancelled';
        setWorkflowState(terminalState);
        setNodes((current) =>
          current.map((node) =>
            node.executionState === 'running'
              ? {
                  ...node,
                  executionState: terminalState === 'cancelled' ? 'idle' : terminalState,
                  executionMessage:
                    terminalState === 'completed'
                      ? undefined
                      : terminalState === 'failed'
                        ? node.executionMessage
                        : 'Workflow cancellation completed.',
                }
              : node,
          ),
        );
        void refreshArtifacts(true);
      }
    },
    [appendLog, project.session_id, refreshArtifacts, resetActivityLog],
  );

  useEffect(() => {
    if (!client) return undefined;
    return client.subscribe((event) => {
      const eventKey = JSON.stringify(event);
      const now = Date.now();
      const previousEvent = recentEventRef.current.get(eventKey);
      if (previousEvent && now - previousEvent < 1000) return;
      recentEventRef.current.set(eventKey, now);
      for (const [key, timestamp] of recentEventRef.current)
        if (now - timestamp >= 1000) recentEventRef.current.delete(key);
      applyWorkflowEvent(event);
    });
  }, [applyWorkflowEvent, client]);

  useEffect(() => {
    if (!client || surface === 'workflow') return undefined;
    void refreshArtifacts(true);
    return undefined;
  }, [client, refreshArtifacts, surface]);

  useEffect(() => {
    if (surface !== 'workflow' || !client) return undefined;
    let active = true;
    const poll = () =>
      client
        .workflowState(project.session_id)
        .then((result) => {
          if (active) {
            workflowExecutionIdRef.current = result.execution_id ?? null;
            setWorkflowState(result.state);
            if (result.progress) setWorkflowProgress(result.progress);
            if (result.state === 'completed' || result.state === 'failed' || result.state === 'cancelled') {
              void refreshArtifacts(true);
            }
          }
        })
        .catch(() => undefined);
    void poll();
    const timer = window.setInterval(() => void poll(), 1000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [appendLog, client, project.session_id, refreshArtifacts, surface]);

  useEffect(() => {
    if (surface !== 'workflow' || !client || !workflowLoaded || typeof client.workflowEvents !== 'function')
      return undefined;
    let active = true;
    client
      .workflowState(project.session_id)
      .then(async (state) => {
        if (!active) return;
        workflowExecutionIdRef.current = state.execution_id ?? null;
        setWorkflowState(state.state);
        if (state.progress) setWorkflowProgress(state.progress);
        resetActivityLog();
        if (!['queued', 'running', 'cancelling'].includes(state.state)) {
          return;
        }
        const result = await client.workflowEvents(project.session_id, 0, 500);
        if (!active) return;
        for (const event of result.events) {
          if (!workflowExecutionIdRef.current || event.execution_id === workflowExecutionIdRef.current)
            applyWorkflowEvent(event);
        }
      })
      .catch(() => undefined);
    return () => {
      active = false;
    };
  }, [appendLog, applyWorkflowEvent, client, project.session_id, resetActivityLog, surface, workflowLoaded]);

  useEffect(() => {
    if (surface !== 'workflow' || !client || typeof client.workflowDefinition !== 'function') return undefined;
    let active = true;
    client
      .workflowDefinition(project.session_id)
      .then((result) => {
        if (!active) return;
        initialViewportFittedRef.current = false;
        setWorkflowRevision(result.workflow.version);
        if (result.workflow.metadata) setWorkflowMetadata(result.workflow.metadata);
        setSavedWorkflowFingerprint(workflowFingerprint(result.workflow));
        if (!result.valid) {
          setStatus(`Saved workflow is invalid: ${result.diagnostics.map((item) => item.message).join('; ')}`);
          setWorkflowLoaded(true);
          return;
        }
        const loadedNodes = result.workflow.operations.map((operation, index) => {
          const capability = capabilities.operations.find((item) => item.canonical_id === operation.operation);
          const position =
            operation.position && Number.isFinite(operation.position.x) && Number.isFinite(operation.position.y)
              ? operation.position
              : workflowLayout(index);
          return {
            id: operation.id,
            kind: 'operation' as const,
            title: capability?.label || operation.operation,
            description: capability?.definition || '',
            x: position.x,
            y: position.y,
            outputsToCanvas: true,
            capabilityId: operation.operation,
            projectEntry: capability?.project_entry === true,
            parameters: uiParameters(capability, operation.parameters),
            executionState: 'idle' as const,
          };
        });
        const loadedEdges = result.workflow.connections.map((connection) => ({
          id: `${connection.source_operation}-${connection.source_port}-${connection.target_operation}-${connection.target_port}`,
          source: connection.source_operation,
          sourcePort: connection.source_port,
          target: connection.target_operation,
          targetPort: connection.target_port,
        }));
        nextId.current = nextCanvasNodeNumber(loadedNodes);
        setNodes(loadedNodes);
        setEdges(loadedEdges);
        const minimumX = Math.min(0, ...loadedNodes.map((node) => node.x));
        const maximumX = Math.max(NODE_WIDTH, ...loadedNodes.map((node) => node.x + NODE_WIDTH));
        const minimumY = Math.min(0, ...loadedNodes.map((node) => node.y));
        const maximumY = Math.max(NODE_HEIGHT, ...loadedNodes.map((node) => node.y + NODE_HEIGHT));
        setWorldSize((current) => ({
          width: Math.max(current.width, maximumX - minimumX + 800),
          height: Math.max(current.height, maximumY - minimumY + 600),
        }));
        setWorkflowLoaded(true);
        setStatus('Workflow loaded from the project.');
      })
      .catch((error) => {
        if (active) {
          setWorkflowLoaded(true);
          setStatus(error instanceof Error ? error.message : 'Workflow could not be loaded.');
        }
      });
    return () => {
      active = false;
    };
  }, [capabilities.operations, client, project.session_id, refreshArtifacts, setStatus, surface, workflowReloadNonce]);

  useLayoutEffect(() => {
    if (!workflowLoaded || !nodes.length || initialViewportFittedRef.current) return undefined;
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const minX = Math.min(...nodes.map((node) => node.x));
    const minY = Math.min(...nodes.map((node) => node.y));
    const maxX = Math.max(...nodes.map((node) => node.x + NODE_WIDTH));
    const maxY = Math.max(...nodes.map((node) => node.y + NODE_HEIGHT));
    const padding = 80;
    const boundsWidth = Math.max(1, maxX - minX);
    const boundsHeight = Math.max(1, maxY - minY);
    const fittedScale = Math.max(
      0.35,
      Math.min(
        1.2,
        (canvas.clientWidth - padding * 2) / boundsWidth,
        (canvas.clientHeight - padding * 2) / boundsHeight,
      ),
    );
    initialViewportFittedRef.current = true;
    setScale(fittedScale);
    setOffset({
      x: (canvas.clientWidth - boundsWidth * fittedScale) / 2 - minX * fittedScale,
      y: (canvas.clientHeight - boundsHeight * fittedScale) / 2 - minY * fittedScale,
    });
    return undefined;
  }, [nodes, workflowLoaded]);

  const workflowAction = async (action: 'run' | 'cancel') => {
    if (!client) return;
    if (action === 'run' && workflowDirty) {
      setStatus('Save the workflow before running it.');
      return;
    }
    setWorkflowBusy(true);
    try {
      const result =
        action === 'run'
          ? await client.runWorkflow(project.session_id)
          : await client.cancelWorkflow(project.session_id);
      if (action === 'run') {
        workflowExecutionIdRef.current = result.execution_id ?? workflowExecutionIdRef.current;
        resetActivityLog();
      }
      setWorkflowState(result.state);
      if (result.progress) setWorkflowProgress(result.progress);
      setStatus(`Workflow ${action} request accepted.`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : `Workflow ${action} failed.`);
    } finally {
      setWorkflowBusy(false);
    }
  };

  const validateCurrentWorkflow = async (): Promise<boolean> => {
    if (!client) return false;
    setWorkflowBusy(true);
    try {
      const result = await client.validateWorkflow(project.session_id, currentWorkflow);
      if (!result.valid) {
        setStatus(`Workflow is invalid: ${result.diagnostics.map((item) => item.message).join('; ')}`);
        setWorkflowState('failed');
        return false;
      }
      setWorkflowState('validated');
      setStatus('Workflow is valid for the current StreamFind installation.');
      return true;
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow validation failed.');
      return false;
    } finally {
      setWorkflowBusy(false);
    }
  };

  const discardWorkflowChanges = () => {
    if (!workflowDirty || workflowBusy) return;
    setStatus('Discarding changes and loading the saved workflow...');
    setWorkflowLoaded(false);
    setWorkflowReloadNonce((value) => value + 1);
  };

  const saveCurrentWorkflow = async (workflowToSave: WorkflowDefinition = currentWorkflow): Promise<boolean> => {
    if (!client || !workflowLoaded) return false;
    setWorkflowBusy(true);
    setSavingWorkflow(true);
    try {
      const validation = await client.validateWorkflow(project.session_id, workflowToSave);
      if (!validation.valid) {
        setStatus(`Workflow was not saved: ${validation.diagnostics.map((item) => item.message).join('; ')}`);
        return false;
      }
      const result = await client.saveWorkflow(project.session_id, workflowToSave);
      setWorkflowRevision(result.workflow.version);
      setSavedWorkflowFingerprint(workflowFingerprint(result.workflow));

      setStatus(`Workflow saved (revision ${result.workflow.version}).`);
      return true;
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow could not be saved.');
      return false;
    } finally {
      setSavingWorkflow(false);
      setWorkflowBusy(false);
    }
  };

  const openWorkflowMetadataEditor = () => {
    setMetadataEditorText(JSON.stringify(workflowMetadata, null, 2));
    setMetadataEditorError(null);
    setMetadataEditorOpen(true);
  };

  const saveWorkflowMetadata = async () => {
    try {
      const parsed = JSON.parse(metadataEditorText) as WorkflowMetadata;
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
        throw new Error('Workflow metadata must be a JSON object.');
      if (client) {
        const validation = await client.validateWorkflow(project.session_id, { ...currentWorkflow, metadata: parsed });
        if (!validation.valid) throw new Error(validation.diagnostics.map((item) => item.message).join('; '));
      }
      const saved = await saveCurrentWorkflow({ ...currentWorkflow, metadata: parsed });
      if (!saved) return;
      setWorkflowMetadata(parsed);
      setMetadataEditorOpen(false);
      setStatus('Workflow metadata updated and saved.');
    } catch (error) {
      setMetadataEditorError(error instanceof Error ? error.message : 'Metadata JSON is invalid.');
    }
  };

  const clearArtifactCache = async () => {
    if (!client || workflowBusy) return;
    setWorkflowBusy(true);
    try {
      await client.clearArtifactCache(project.session_id);
      historyRef.current = { past: [], current: historyRef.current.current, future: [] };
      setStatus('Cached data and cache history were cleared; published artifacts were retained.');
      await refreshArtifacts(true);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Cached data could not be cleared.');
    } finally {
      setWorkflowBusy(false);
    }
  };

  const clearAllArtifacts = async () => {
    if (!client || workflowBusy) return;
    setWorkflowBusy(true);
    try {
      await client.clearAllArtifacts(project.session_id);
      setStatus('All artifacts and cache entries were cleared; the workflow definition was retained.');
      await refreshArtifacts(true);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Artifacts could not be cleared.');
    } finally {
      setWorkflowBusy(false);
    }
  };

  const resetWorkflow = () => {
    if (workflowBusy) return;
    nextId.current = 1;
    setNodes([]);
    setEdges([]);
    setExpandedParameters({});
    setSelectedNodeId(null);
    setSelectedEdgeId(null);
    setWorkflowState('idle');
    setWorkflowProgress({ completed: 0, total: 0, current_step: 0 });
    setStatus('Workflow reset to an empty definition. Save it if desired.');
  };

  const exportWorkflow = () => {
    const blob = new Blob([JSON.stringify(currentWorkflow, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = 'streamfind-workflow.json';
    link.click();
    URL.revokeObjectURL(url);
    setStatus('Workflow exported as streamfind-workflow.json.');
  };

  const applyImportedWorkflow = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file || !client) return;
    setWorkflowBusy(true);
    try {
      const parsed = JSON.parse(await file.text()) as Partial<WorkflowDefinition>;
      if (parsed.schema_version !== 1 || !Array.isArray(parsed.operations) || !Array.isArray(parsed.connections)) {
        throw new Error('The selected file is not a compatible StreamFind workflow schema.');
      }
      const imported: WorkflowDefinition = {
        schema_version: 1,
        workflow_id: parsed.workflow_id,
        name: parsed.name,
        version: workflowRevision,
        metadata: parsed.metadata || workflowMetadata,
        operations: parsed.operations,
        connections: parsed.connections,
      };
      const validation = await client.validateWorkflow(project.session_id, imported);
      if (!validation.valid) {
        setStatus(`Workflow was not loaded: ${validation.diagnostics.map((item) => item.message).join('; ')}`);
        return;
      }
      const { nodes: loadedNodes, edges: loadedEdges } = workflowToCanvas(imported, capabilities);
      nextId.current = nextCanvasNodeNumber(loadedNodes);
      setNodes(loadedNodes);
      setEdges(loadedEdges);
      const minimumX = Math.min(0, ...loadedNodes.map((node) => node.x));
      const maximumX = Math.max(NODE_WIDTH, ...loadedNodes.map((node) => node.x + NODE_WIDTH));
      const minimumY = Math.min(0, ...loadedNodes.map((node) => node.y));
      const maximumY = Math.max(NODE_HEIGHT, ...loadedNodes.map((node) => node.y + NODE_HEIGHT));
      setWorldSize((current) => ({
        width: Math.max(current.width, maximumX - minimumX + 800),
        height: Math.max(current.height, maximumY - minimumY + 600),
      }));
      setExpandedParameters({});
      setWorkflowState('validated');
      setStatus('Workflow imported. Save it to persist it in the project.');
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Workflow could not be imported.');
    } finally {
      setWorkflowBusy(false);
    }
  };

  useEffect(() => {
    if (surface !== 'workflow' || !client || !['queued', 'running', 'cancelling'].includes(workflowState))
      return undefined;
    const timer = window.setInterval(() => {
      client
        .workflowState(project.session_id)
        .then((result) => {
          setWorkflowState(result.state);
          if (result.progress) setWorkflowProgress(result.progress);
        })
        .catch(() => undefined);
    }, 500);
    return () => window.clearInterval(timer);
  }, [client, project.session_id, surface, workflowState]);

  const nodeMap = useMemo(() => new Map(nodes.map((node) => [node.id, node])), [nodes]);
  const toWorld = (event: ReactMouseEvent): Point => {
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return { x: (event.clientX - rect.left - offset.x) / scale, y: (event.clientY - rect.top - offset.y) / scale };
  };
  const nodeCapability = (node: CanvasNode) =>
    node.capabilityId ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId) : undefined;
  // This layout helper intentionally tracks the latest measured anchor positions.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const positionedPort = (node: CanvasNode, portId: string, direction: 'input' | 'output'): Point =>
    portPosition(
      node,
      nodeCapability(node),
      portId,
      direction,
      anchorCenters[`${node.id}|${direction}|${portId}`],
      expandedParameters[node.id] ?? node.projectEntry,
    );
  const typedPort = (node: CanvasNode, portId: string, direction: 'input' | 'output'): string =>
    edgeTypeKey(nodeCapability(node), portId, direction);

  useEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const canvasRect = canvas.getBoundingClientRect();
      const next: Record<string, Point> = {};
      document.querySelectorAll<HTMLElement>('[data-canvas-anchor]').forEach((element) => {
        const key = element.dataset.canvasAnchor;
        if (!key) return;
        const rect = element.getBoundingClientRect();
        next[key] = {
          x: (rect.left + rect.width / 2 - canvasRect.left - offset.x) / scale,
          y: (rect.top + rect.height / 2 - canvasRect.top - offset.y) / scale,
        };
      });
      document.querySelectorAll<HTMLElement>('[data-canvas-parameter-summary]').forEach((element) => {
        const nodeId = element.dataset.canvasParameterSummary;
        const node = nodeId ? nodeMap.get(nodeId) : undefined;
        const capability = node?.capabilityId
          ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId)
          : undefined;
        if (!node || !capability) return;
        const rect = element.getBoundingClientRect();
        const point = {
          x: (rect.left + rect.width / 2 - canvasRect.left - offset.x) / scale,
          y: (rect.top + rect.height / 2 - canvasRect.top - offset.y) / scale,
        };
        capability.parameters.forEach((parameter) => {
          next[`${node.id}|input|parameter:${parameter.name}`] = point;
        });
      });
      const previous = anchorCentersRef.current;
      const keys = Object.keys(next);
      const unchanged =
        keys.length === Object.keys(previous).length &&
        keys.every((key) => previous[key]?.x === next[key].x && previous[key]?.y === next[key].y);
      if (!unchanged) {
        anchorCentersRef.current = next;
        setAnchorCenters(next);
      }
    });
    return () => window.cancelAnimationFrame(frame);
  }, [capabilities.operations, expandedParameters, nodeMap, nodes, offset, scale]);

  useEffect(() => {
    const move = (event: globalThis.MouseEvent) => {
      if (interaction?.type === 'pan') {
        setOffset({ x: event.clientX - (interaction.originX || 0), y: event.clientY - (interaction.originY || 0) });
        return;
      }
      const rightDrag = rightDragRef.current;
      if (rightDrag && !rightDrag.moved) {
        const movedX = event.clientX - rightDrag.start.x;
        const movedY = event.clientY - rightDrag.start.y;
        if (Math.hypot(movedX, movedY) >= 4) {
          rightDrag.moved = true;
          suppressContextMenuRef.current = true;
          const start = rightDrag.start;
          setInteraction({ type: 'marquee', startX: start.x, startY: start.y });
        }
      }
      if (rightDragRef.current?.moved) {
        const rect = canvasRef.current?.getBoundingClientRect();
        if (rect) {
          const start = rightDragRef.current.start;
          const current = {
            x: (event.clientX - rect.left - offset.x) / scale,
            y: (event.clientY - rect.top - offset.y) / scale,
          };
          const x = Math.min(start.x, current.x);
          const y = Math.min(start.y, current.y);
          const width = Math.abs(current.x - start.x);
          const height = Math.abs(current.y - start.y);
          setSelectionBox({ x, y, width, height });
          setSelectedNodeIds(
            nodesRef.current
              .filter(
                (node) =>
                  node.x < x + width && node.x + NODE_WIDTH > x && node.y < y + height && node.y + NODE_HEIGHT > y,
              )
              .map((node) => node.id),
          );
        }
      }
      if (interaction?.type === 'node' && interaction.id) {
        const draggedNodeId = interaction.id;
        dragPointerRef.current = { clientX: event.clientX, clientY: event.clientY };
        if (dragFrameRef.current === null) {
          dragFrameRef.current = window.requestAnimationFrame(() => {
            dragFrameRef.current = null;
            const pointer = dragPointerRef.current;
            if (!pointer) return;
            const rect = canvasRef.current?.getBoundingClientRect();
            if (!rect) return;
            let x = (pointer.clientX - rect.left - offset.x) / scale - (interaction.offsetX || 0);
            let y = (pointer.clientY - rect.top - offset.y) / scale - (interaction.offsetY || 0);
            if (gridVisible) {
              x = Math.round(x / GRID_SIZE) * GRID_SIZE;
              y = Math.round(y / GRID_SIZE) * GRID_SIZE;
            }
            const draggedOrigin = interaction.origins?.[draggedNodeId] ?? { x, y };
            const deltaX = x - draggedOrigin.x;
            const deltaY = y - draggedOrigin.y;
            const selectedOrigins = Object.values(interaction.origins || {});
            const minimumX = selectedOrigins.length
              ? Math.min(...selectedOrigins.map((origin) => origin.x + deltaX))
              : x;
            const minimumY = selectedOrigins.length
              ? Math.min(...selectedOrigins.map((origin) => origin.y + deltaY))
              : y;
            // Grow the canvas when a drag reaches an edge.  For the top/left
            // edges, moving the world origin creates visible space in that
            // direction; the node remains under the pointer and is not
            // clamped at the old boundary.
            const edgeMargin = 240;
            const minimumNodeX = selectedOrigins.length ? minimumX : x;
            const minimumNodeY = selectedOrigins.length ? minimumY : y;
            const proposedMaximumX = selectedOrigins.length
              ? Math.max(...selectedOrigins.map((origin) => origin.x + deltaX + NODE_WIDTH))
              : x + NODE_WIDTH;
            const proposedMaximumY = selectedOrigins.length
              ? Math.max(...selectedOrigins.map((origin) => origin.y + deltaY + NODE_HEIGHT))
              : y + NODE_HEIGHT;
            const currentWorld = worldSizeRef.current;
            const edges = {
              left: minimumNodeX < edgeMargin,
              right: proposedMaximumX > currentWorld.width - edgeMargin,
              top: minimumNodeY < edgeMargin,
              bottom: proposedMaximumY > currentWorld.height - edgeMargin,
            };
            // Keep an expansion allocated to an edge. It is re-armed only
            // after the dragged group has moved well clear of that edge;
            // releasing and grabbing the same node must not grow/jump again.
            const resetMargin = edgeMargin * 2;
            if (minimumNodeX > resetMargin) dragExpandedEdgesRef.current.left = false;
            if (proposedMaximumX < currentWorld.width - resetMargin) dragExpandedEdgesRef.current.right = false;
            if (minimumNodeY > resetMargin) dragExpandedEdgesRef.current.top = false;
            if (proposedMaximumY < currentWorld.height - resetMargin) dragExpandedEdgesRef.current.bottom = false;
            const growWidth =
              (edges.left && !dragExpandedEdgesRef.current.left) ||
              (edges.right && !dragExpandedEdgesRef.current.right);
            const growHeight =
              (edges.top && !dragExpandedEdgesRef.current.top) ||
              (edges.bottom && !dragExpandedEdgesRef.current.bottom);
            if (edges.left) dragExpandedEdgesRef.current.left = true;
            if (edges.right) dragExpandedEdgesRef.current.right = true;
            if (edges.top) dragExpandedEdgesRef.current.top = true;
            if (edges.bottom) dragExpandedEdgesRef.current.bottom = true;
            const rightGrowth = growWidth ? Math.ceil((currentWorld.width * 0.3) / GRID_SIZE) * GRID_SIZE : 0;
            const bottomGrowth = growHeight ? Math.ceil((currentWorld.height * 0.3) / GRID_SIZE) * GRID_SIZE : 0;
            const nextWorld = {
              width: currentWorld.width + rightGrowth,
              height: currentWorld.height + bottomGrowth,
            };
            if (rightGrowth || bottomGrowth) {
              worldSizeRef.current = nextWorld;
              setWorldSize(nextWorld);
            }
            // The world grows by 30%, but only a small amount of that new
            // space needs to be revealed immediately.  Moving by the full
            // growth amount makes the whole workflow jump far away from the
            // pointer and produces very long connector segments.
            const edgeRevealDistance = 240;
            if (edges.left && growWidth) {
              setOffset((current) => ({ ...current, x: current.x + edgeRevealDistance * scale }));
            }
            if (edges.top && growHeight) {
              setOffset((current) => ({ ...current, y: current.y + edgeRevealDistance * scale }));
            }
            setNodes((current) =>
              current.map((node) => {
                const origin = interaction.origins?.[node.id];
                if (!origin) return node.id === draggedNodeId ? { ...node, x, y } : node;
                return {
                  ...node,
                  x: origin.x + deltaX,
                  y: origin.y + deltaY,
                };
              }),
            );
          });
        }
      }
      const pendingConnection = pendingConnectionRef.current;
      if (!connection && pendingConnection) {
        const movedX = event.clientX - pendingConnection.startX;
        const movedY = event.clientY - pendingConnection.startY;
        if (Math.hypot(movedX, movedY) >= 4) {
          const source = nodeMap.get(pendingConnection.source);
          if (source) {
            pendingConnectionRef.current = null;
            setConnection({
              source: pendingConnection.source,
              sourcePort: pendingConnection.sourcePort,
              point: positionedPort(source, pendingConnection.sourcePort, 'output'),
            });
            setStatus('Drag this output to a compatible input on another operation.');
          }
        }
      }
      if (connection) {
        const rect = canvasRef.current?.getBoundingClientRect();
        if (rect)
          setConnection({
            ...connection,
            point: {
              x: (event.clientX - rect.left - offset.x) / scale,
              y: (event.clientY - rect.top - offset.y) / scale,
            },
          });
      }
    };
    const up = () => {
      if (rightDragRef.current?.moved) setSelectionBox(null);
      pendingConnectionRef.current = null;
      setInteraction(null);
      rightDragRef.current = null;
    };
    const blur = () => {
      if (dragFrameRef.current !== null) window.cancelAnimationFrame(dragFrameRef.current);
      dragFrameRef.current = null;
      rightDragRef.current = null;
      setSelectionBox(null);
      setInteraction(null);
      setConnection(null);
    };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
    window.addEventListener('blur', blur);
    return () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
      window.removeEventListener('blur', blur);
    };
  }, [connection, gridVisible, interaction, nodeMap, offset, positionedPort, scale, setStatus]);

  useEffect(() => {
    const centerProject = () => {
      const canvas = canvasRef.current;
      const entry = nodes[0];
      if (!canvas || !entry) return;
      setOffset({
        x: canvas.clientWidth / 2 - (entry.x + NODE_WIDTH / 2) * scale,
        y: canvas.clientHeight / 2 - (entry.y + NODE_HEIGHT / 2) * scale,
      });
    };
    const frame = window.requestAnimationFrame(centerProject);
    return () => window.cancelAnimationFrame(frame);
    // The initial project node is centered once; dragging must not recenter the canvas.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const handleWheel = (event: WheelEvent) => {
      if (
        (event.target as HTMLElement).closest(
          '.sf-canvas-toolbar, .sf-canvas-picker, .sf-canvas-status, .sf-node-info-pane, .sf-json-editor-overlay, .sf-artifact-viewer-backdrop',
        )
      )
        return;
      event.preventDefault();
      if (!event.ctrlKey && !event.metaKey) {
        setOffset((current) => ({ x: current.x - event.deltaX, y: current.y - event.deltaY }));
        return;
      }
      const rect = canvas.getBoundingClientRect();
      const cursorX = event.clientX - rect.left;
      const cursorY = event.clientY - rect.top;
      const { offset: currentOffset, scale: currentScale } = viewportRef.current;
      const worldX = (cursorX - currentOffset.x) / currentScale;
      const worldY = (cursorY - currentOffset.y) / currentScale;
      const nextScale = Math.min(2.8, Math.max(0.35, currentScale * Math.exp(-event.deltaY * 0.0015)));
      setScale(nextScale);
      setOffset({ x: cursorX - worldX * nextScale, y: cursorY - worldY * nextScale });
    };
    canvas.addEventListener('wheel', handleWheel, { passive: false });
    return () => canvas.removeEventListener('wheel', handleWheel);
  }, []);

  const beginPan = (event: ReactMouseEvent) => {
    const target = event.target as HTMLElement;
    if (target.closest('.sf-canvas-node, .sf-canvas-toolbar, .sf-canvas-picker')) return;
    if (event.button === 2) {
      rightDragRef.current = { start: toWorld(event), moved: false };
      setSelectionBox(null);
      return;
    }
    if (event.button !== 0 && event.button !== 1) return;
    setSelectedNodeIds([]);
    setSelectedEdgeId(null);
    setSelectedNodeId(null);
    setDocumentationFocus(null);
    setPicker(null);
    setInteraction({ type: 'pan', originX: event.clientX - offset.x, originY: event.clientY - offset.y });
  };
  const beginRightSelection = (event: ReactMouseEvent) => {
    event.stopPropagation();
    if (event.button !== 2) return;
    rightDragRef.current = { start: toWorld(event), moved: false };
    setSelectionBox(null);
  };
  const beginNodeDrag = (event: ReactMouseEvent, node: CanvasNode) => {
    event.stopPropagation();
    if (event.button === 2) {
      beginRightSelection(event);
      return;
    }
    const point = toWorld(event);
    setPicker(null);
    const selectedIds = selectedNodeIds.includes(node.id) ? selectedNodeIds : [node.id];
    const origins = Object.fromEntries(
      nodes
        .filter((candidate) => selectedIds.includes(candidate.id))
        .map((candidate) => [candidate.id, { x: candidate.x, y: candidate.y }]),
    );
    setSelectedNodeIds(selectedIds);
    setInteraction({
      type: 'node',
      id: node.id,
      selectedIds,
      origins,
      offsetX: point.x - node.x,
      offsetY: point.y - node.y,
    });
  };
  const beginConnection = (event: ReactMouseEvent, node: CanvasNode, sourcePort: string) => {
    event.stopPropagation();
    setPicker(null);
    setConnection(null);
    pendingConnectionRef.current = { source: node.id, sourcePort, startX: event.clientX, startY: event.clientY };
  };
  const canConnect = (source: CanvasNode, target: CanvasNode, targetPort: string): boolean => {
    const sourcePorts = nodePorts(nodeCapability(source)).outputs;
    const targetPorts = nodePorts(nodeCapability(target)).inputs;
    const targetCapability = nodeCapability(target);
    const parameterName = targetPort.startsWith('parameter:') ? targetPort.slice('parameter:'.length) : '';
    const sourcePort = sourcePorts.find((port) => port.id === connection?.sourcePort);
    const targetInput = targetPorts.find((port) => port.id === targetPort);
    const targetParameter = targetCapability?.parameters.find((parameter) => parameter.name === parameterName);
    const compatibleTypes = sourcePort && targetInput ? portMatchesPort(sourcePort, targetInput) : false;
    const parameterCompatible =
      sourcePort && targetParameter ? portMatchesParameter(sourcePort, targetParameter) : false;
    return sourcePort !== undefined && (compatibleTypes || parameterCompatible);
  };
  const finishConnection = (event: ReactMouseEvent, target: CanvasNode, targetPort: string) => {
    event.stopPropagation();
    if (!connection || connection.source === target.id) return;
    const source = nodeMap.get(connection.source);
    if (!source || !canConnect(source, target, targetPort)) {
      setConnection(null);
      setStatus('Only operation output connectors can connect to compatible operation inputs.');
      return;
    }
    const edgeId = `${connection.source}-${connection.sourcePort}-${target.id}-${targetPort}`;
    setEdges((current) =>
      current.some((edge) => edge.id === edgeId)
        ? current
        : [
            ...current,
            {
              id: edgeId,
              source: connection.source,
              sourcePort: connection.sourcePort,
              target: target.id,
              targetPort,
            },
          ],
    );
    setConnection(null);
    setPicker(null);
    setStatus('Connected workflow nodes.');
  };
  const finishOnCanvas = (event: ReactMouseEvent) => {
    if (!connection) return;
    setPicker({ ...toWorld(event), source: connection.source, sourcePort: connection.sourcePort });
    setConnection(null);
  };
  const beginEdgeReconnect = (event: ReactMouseEvent, edge: CanvasEdge) => {
    event.stopPropagation();
    setSelectedEdgeId(edge.id);
    setEdges((current) => current.filter((candidate) => candidate.id !== edge.id));
    const source = nodeMap.get(edge.source);
    if (source) {
      setConnection({
        source: edge.source,
        sourcePort: edge.sourcePort,
        point: positionedPort(source, edge.sourcePort, 'output'),
      });
      setStatus('Move the selected connector to a compatible input, or release on the canvas to choose a new node.');
    }
  };
  const deleteNode = (nodeId: string) => {
    setNodes((current) => current.filter((node) => node.id !== nodeId));
    setEdges((current) => current.filter((edge) => edge.source !== nodeId && edge.target !== nodeId));
    setSelectedNodeId(null);
    setStatus('Deleted node and its connected connectors.');
  };
  const requestClearNodeArtifacts = (node: CanvasNode) => {
    if (!client || nodeControlsLocked) return;
    setArtifactCleanupNode(node);
  };
  const clearNodeArtifacts = async (node: CanvasNode) => {
    if (!client || nodeControlsLocked) return;
    setArtifactCleanupNode(null);
    setArtifactCleanupBusy(true);
    setWorkflowBusy(true);
    try {
      await client.clearNodeArtifacts(project.session_id, node.id);
      await refreshArtifacts(true);
      setStatus(`Deleted published output data for ${node.title}.`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Node artifact cleanup failed.');
    } finally {
      setArtifactCleanupBusy(false);
      setWorkflowBusy(false);
    }
  };
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Delete' && event.key !== 'Backspace') return;
      const target = event.target as HTMLElement;
      if (target.matches('input, textarea, select, [contenteditable="true"]')) return;
      if (selectedEdgeId) {
        event.preventDefault();
        setEdges((current) => current.filter((edge) => edge.id !== selectedEdgeId));
        setSelectedEdgeId(null);
        setStatus('Deleted connector.');
      } else if (selectedNodeIds.length || selectedNodeId) {
        event.preventDefault();
        const ids = new Set(selectedNodeIds.length ? selectedNodeIds : selectedNodeId ? [selectedNodeId] : []);
        setNodes((current) => current.filter((node) => !ids.has(node.id)));
        setEdges((current) => current.filter((edge) => !ids.has(edge.source) && !ids.has(edge.target)));
        setSelectedNodeIds([]);
        setSelectedNodeId(null);
        setStatus('Deleted node and its connected connectors.');
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [selectedEdgeId, selectedNodeId, selectedNodeIds, setStatus]);
  useEffect(() => {
    if (!selectedNodeId && !pickerCapabilityId) return undefined;
    const handleDocumentationKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      setSelectedNodeId(null);
      setPickerCapabilityId(null);
    };
    window.addEventListener('keydown', handleDocumentationKeyDown);
    return () => window.removeEventListener('keydown', handleDocumentationKeyDown);
  }, [pickerCapabilityId, selectedNodeId]);
  const addNode = (template: NodeTemplate) => {
    if (!picker) return;
    const id = `${template.kind}-${nextId.current++}`;
    const source = picker.source;
    const sourceNode = source ? nodeMap.get(source) : undefined;
    const position = sourceNode
      ? connectedNodePosition(sourceNode, nodes)
      : { x: picker.x - NODE_WIDTH / 2, y: picker.y - NODE_HEIGHT / 2 };
    const node: CanvasNode = {
      id,
      kind: template.kind,
      title: template.title,
      description: template.description,
      x: position.x,
      y: position.y,
      outputsToCanvas: template.outputsToCanvas,
      capabilityId: template.capabilityId,
      projectEntry: template.projectEntry,
      parameters: template.capabilityId
        ? defaultParameters(
            capabilities.operations.find((item) => item.canonical_id === template.capabilityId) as BackendCapability,
          )
        : {},
    };
    setNodes((current) => [...current, node]);
    if (source) {
      const targetCapability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      const sourcePort =
        sourceNode && nodePorts(nodeCapability(sourceNode)).outputs.find((port) => port.id === picker.sourcePort);
      const targetPort =
        nodePorts(targetCapability).inputs.find((port) => sourcePort && portMatchesPort(sourcePort, port))?.id ||
        (sourcePort
          ? targetCapability?.parameters.find(
              (parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter),
            )?.name
            ? `parameter:${targetCapability.parameters.find((parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter))?.name}`
            : ''
          : '');
      if (!targetPort) return;
      setEdges((current) => [
        ...current,
        { id: `${source}-${id}`, source, sourcePort: picker.sourcePort || '', target: id, targetPort },
      ]);
    }
    setPicker(null);
    setStatus(`Added ${template.title} node.`);
  };
  const availableTemplates = useMemo(() => {
    const standaloneTemplates = nodeTemplates;
    if (!picker?.source) return standaloneTemplates;
    const source = nodeMap.get(picker.source);
    if (!source) return standaloneTemplates;
    const sourceCapability = source.capabilityId
      ? capabilities.operations.find((item) => item.canonical_id === source.capabilityId)
      : undefined;
    const sourcePort = nodePorts(sourceCapability).outputs.find((port) => port.id === picker.sourcePort);
    return standaloneTemplates.filter((template) => {
      const targetCapability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      if (source.kind === 'method') return template.kind === 'method';
      if (source.kind === 'operation')
        return (
          (template.kind === 'operation' || template.kind === 'extract' || template.kind === 'plot') &&
          Boolean(
            sourcePort &&
            (nodePorts(targetCapability).inputs.some((port) => portMatchesPort(sourcePort, port)) ||
              targetCapability?.parameters.some(
                (parameter) => parameter.name !== 'database_path' && portMatchesParameter(sourcePort, parameter),
              )),
          )
        );
      return false;
    });
  }, [capabilities.operations, nodeMap, nodeTemplates, picker]);
  const paletteModules = useMemo(
    () =>
      [
        ...new Set(
          availableTemplates.map((template) => template.module).filter((module): module is string => Boolean(module)),
        ),
      ].sort(),
    [availableTemplates],
  );
  const paletteSearchQuery = paletteSearch.trim().toLocaleLowerCase();
  const filteredTemplates = useMemo(() => {
    const categorized = availableTemplates.filter((template) => !paletteModule || template.module === paletteModule);
    if (!paletteSearchQuery) return categorized;
    return categorized.filter((template) => {
      const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      const ontologyFragment = [
        capability?.canonical_id,
        capability?.label,
        capability?.definition,
        capability?.domain,
        capability?.module_id,
      ]
        .filter((value): value is string => typeof value === 'string')
        .join(' ')
        .toLocaleLowerCase();
      return ontologyFragment.includes(paletteSearchQuery);
    });
  }, [availableTemplates, capabilities.operations, paletteModule, paletteSearchQuery]);
  const arrangeNodes = () => {
    if (!nodes.length) return;
    const indegree = new Map(nodes.map((node) => [node.id, 0]));
    const outgoing = new Map<string, string[]>();
    const incoming = new Map<string, string[]>();
    edges.forEach((edge) => {
      indegree.set(edge.target, (indegree.get(edge.target) || 0) + 1);
      outgoing.set(edge.source, [...(outgoing.get(edge.source) || []), edge.target]);
      incoming.set(edge.target, [...(incoming.get(edge.target) || []), edge.source]);
    });
    const queue = nodes.filter((node) => (indegree.get(node.id) || 0) === 0).map((node) => node.id);
    const depth = new Map(queue.map((id) => [id, 0]));
    for (let index = 0; index < queue.length; index += 1) {
      const current = queue[index];
      for (const next of outgoing.get(current) || []) {
        depth.set(next, Math.max(depth.get(next) || 0, (depth.get(current) || 0) + 1));
        const remaining = (indegree.get(next) || 0) - 1;
        indegree.set(next, remaining);
        if (remaining === 0) queue.push(next);
      }
    }
    nodes.forEach((node) => {
      if (!depth.has(node.id)) depth.set(node.id, 0);
    });
    const groups = new Map<number, CanvasNode[]>();
    nodes.forEach((node) =>
      groups.set(depth.get(node.id) || 0, [...(groups.get(depth.get(node.id) || 0) || []), node]),
    );
    const arranged = nodes.map((node) => ({ ...node }));
    const byId = new Map(arranged.map((node) => [node.id, node]));
    const stages = Array.from(groups.keys()).sort((a, b) => a - b);
    const maxStage = stages[stages.length - 1] || 0;
    const largestColumn = Math.max(...stages.map((stage) => groups.get(stage)?.length || 0), 1);
    const stageStep = NODE_WIDTH + 236;
    const rowStep = NODE_HEIGHT + 140;
    const layoutWidth = Math.max(CANVAS_WORLD_WIDTH, (maxStage + 1) * stageStep + NODE_WIDTH + 900);
    const layoutHeight = Math.max(CANVAS_WORLD_HEIGHT, largestColumn * rowStep + 1000);
    const centerX = layoutWidth / 2;
    const centerY = layoutHeight / 2;
    stages.forEach((stage) => {
      const column = groups.get(stage) || [];
      const parentCenter = (node: CanvasNode) => {
        const parents = (incoming.get(node.id) || [])
          .map((parentId) => byId.get(parentId))
          .filter((parent): parent is CanvasNode => Boolean(parent));
        if (!parents.length) return Number.POSITIVE_INFINITY;
        return parents.reduce((sum, parent) => sum + parent.y + NODE_HEIGHT / 2, 0) / parents.length;
      };
      column.sort((a, b) => parentCenter(a) - parentCenter(b) || a.y - b.y || a.id.localeCompare(b.id));
      column.forEach((node, row) => {
        const target = byId.get(node.id);
        if (!target) return;
        target.x = centerX + (stage - maxStage / 2) * stageStep - NODE_WIDTH / 2;
        target.y = centerY + (row - (column.length - 1) / 2) * rowStep - NODE_HEIGHT / 2;
      });
    });
    setWorldSize((current) => ({
      width: Math.max(current.width, layoutWidth),
      height: Math.max(current.height, layoutHeight),
    }));
    setNodes(arranged);
    setScale(1);
    const minX = Math.min(...arranged.map((node) => node.x));
    const minY = Math.min(...arranged.map((node) => node.y));
    const maxX = Math.max(...arranged.map((node) => node.x + NODE_WIDTH));
    const maxY = Math.max(...arranged.map((node) => node.y + NODE_HEIGHT));
    const canvas = canvasRef.current;
    if (canvas)
      setOffset({ x: canvas.clientWidth / 2 - (minX + maxX) / 2, y: canvas.clientHeight / 2 - (minY + maxY) / 2 });
    setStatus('Arranged nodes by dependency stage with spaced dependency branches.');
  };
  const zoom = (amount: number) =>
    setScale((current) => Math.min(2.8, Math.max(0.35, Number((current + amount).toFixed(2)))));
  const openStandalonePicker = () => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    setPicker({
      x: (canvas.clientWidth / 2 - offset.x) / scale,
      y: (canvas.clientHeight / 2 - offset.y) / scale,
    });
  };
  const updateNodeParameter = (nodeId: string, parameterName: string, value: unknown) => {
    setNodes((current) =>
      current.map((node) =>
        node.id === nodeId ? { ...node, parameters: { ...(node.parameters || {}), [parameterName]: value } } : node,
      ),
    );
  };
  const openJsonEditor = (node: CanvasNode, parameter: CapabilityParameter) => {
    setJsonEditor({ nodeId: node.id, parameter });
    setJsonEditorText(
      JSON.stringify(
        node.parameters?.[parameter.name] ??
          (parameter.schema.type === 'array' || parameter.schema.type === 'table' ? [] : {}),
        null,
        2,
      ),
    );
    setJsonEditorError(null);
  };
  const openPathWizard = (node: CanvasNode, parameter: CapabilityParameter, mergeIntoJsonEditor = false) => {
    setPathWizard({ nodeId: node.id, parameter, mergeIntoJsonEditor });
  };
  const openPathWizardFromJsonEditor = () => {
    if (!jsonEditor) return;
    const node = nodes.find((candidate) => candidate.id === jsonEditor.nodeId);
    if (node) openPathWizard(node, jsonEditor.parameter, true);
  };
  const saveJsonEditor = () => {
    if (!jsonEditor) return;
    try {
      const parsed = JSON.parse(jsonEditorText);
      const normalized =
        jsonEditor.parameter.schema.type === 'table' && Array.isArray(parsed)
          ? normalizeInlineTableRows(jsonEditor.parameter, parsed as Record<string, unknown>[])
          : parsed;
      const validationError = validateJsonValue(normalized, jsonEditor.parameter.schema);
      if (validationError) {
        setJsonEditorError(validationError);
        return;
      }
      updateNodeParameter(jsonEditor.nodeId, jsonEditor.parameter.name, normalized);
      setJsonEditor(null);
      setJsonEditorError(null);
    } catch (error) {
      setJsonEditorError(error instanceof Error ? error.message : 'Invalid JSON.');
    }
  };
  const importJsonCsv = async (file: File) => {
    if (!jsonEditor) return;
    const schema = jsonEditor.parameter.schema;
    const itemSchema = schema.items;
    const rowSchema = schema.properties ? schema : itemSchema;
    const columns = rowSchema?.properties ? schemaPropertyOrder(rowSchema) : [];
    if (!columns.length) {
      setJsonEditorError('CSV import requires an array of structured rows with declared columns.');
      return;
    }
    const parsed = parseCsvRows(await file.text(), columns);
    if (parsed.error || !parsed.rows) {
      setJsonEditorError(parsed.error || 'CSV could not be parsed.');
      return;
    }
    setJsonEditorText(JSON.stringify(parsed.rows, null, 2));
    setJsonEditorError(null);
  };
  const addSelectedPaths = async (node: CanvasNode, parameter: CapabilityParameter, folders: boolean) => {
    if (!client) return;
    try {
      const paths = folders ? await client.pickFolders() : await client.pickFiles(fileParameterExtensions(parameter));
      const current: unknown[] = Array.isArray(node.parameters?.[parameter.name])
        ? (node.parameters[parameter.name] as unknown[])
        : [];
      const existing = new Set(
        current.map((item) =>
          typeof item === 'string'
            ? item
            : typeof item === 'object' && item !== null
              ? String((item as Record<string, unknown>).path || '')
              : '',
        ),
      );
      const added = paths
        .filter((path) => !existing.has(path))
        .map((path) => (isSimplePathParameter(parameter) ? path : { path }));
      updateNodeParameter(node.id, parameter.name, [...current, ...added]);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const chooseSinglePath = async (node: CanvasNode, parameter: CapabilityParameter) => {
    if (!client) return;
    try {
      const paths =
        parameter.path_kind === 'directory'
          ? await client.pickFolders()
          : parameter.path_kind === 'file-output'
            ? [await client.pickSaveFile()]
            : await client.pickFiles(fileParameterExtensions(parameter));
      if (paths[0]) updateNodeParameter(node.id, parameter.name, paths[0]);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const openTableEditorFromJsonEditor = () => {
    if (!jsonEditor) return;
    try {
      const parsed = JSON.parse(jsonEditorText);
      if (!Array.isArray(parsed)) throw new Error('Table JSON must be an array of rows.');
      const rows = parsed.filter(
        (row): row is Record<string, unknown> => typeof row === 'object' && row !== null && !Array.isArray(row),
      );
      if (rows.length !== parsed.length) throw new Error('Every table row must be a JSON object.');
      setTableEditor({
        nodeId: jsonEditor.nodeId,
        parameter: jsonEditor.parameter,
        rows,
        returnToJsonEditor: true,
        error: null,
      });
      setJsonEditor(null);
    } catch (error) {
      setJsonEditorError(error instanceof Error ? error.message : 'Invalid table JSON.');
    }
  };
  const updateTableCell = (rowIndex: number, columnName: string, value: string) => {
    setTableEditor((current) => {
      if (!current) return current;
      const rows = current.rows.map((row, index) => (index === rowIndex ? { ...row, [columnName]: value } : row));
      return { ...current, rows, error: null };
    });
  };
  const chooseTableColumnPaths = async (columnName: string, schema: JsonSchema, folders = false) => {
    if (!client || !tableEditor) return;
    const declared = schema.extensions;
    const extensions = Array.isArray(declared)
      ? declared.filter((item): item is string => typeof item === 'string')
      : [];
    try {
      const paths = folders ? await client.pickFolders() : await client.pickFiles(extensions);
      const columns = schemaPropertyOrder(tableEditor.parameter.schema);
      const rows = [
        ...tableEditor.rows,
        ...paths.map(
          (path) =>
            Object.fromEntries(columns.map((column) => [column, column === columnName ? path : ''])) as Record<
              string,
              unknown
            >,
        ),
      ];
      setTableEditor({ ...tableEditor, rows });
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const saveTableEditor = () => {
    if (!tableEditor) return;
    const error = validateInlineTableRows(tableEditor.parameter, tableEditor.rows);
    if (error) {
      setTableEditor((current) => (current ? { ...current, error } : current));
      return;
    }
    const normalized = normalizeInlineTableRows(tableEditor.parameter, tableEditor.rows);
    if (tableEditor.returnToJsonEditor) {
      setJsonEditor({ nodeId: tableEditor.nodeId, parameter: tableEditor.parameter });
      setJsonEditorText(JSON.stringify(normalized, null, 2));
      setJsonEditorError(null);
      setTableEditor(null);
      return;
    }
    updateNodeParameter(tableEditor.nodeId, tableEditor.parameter.name, normalized);
    setTableEditor(null);
  };
  const importTableCsv = async (file: File) => {
    if (!tableEditor) return;
    const parsed = parseCsvRows(await file.text(), schemaPropertyOrder(tableEditor.parameter.schema));
    if (parsed.error || !parsed.rows) {
      setCsvPreview({ rows: [], error: parsed.error || 'CSV could not be parsed.' });
      return;
    }
    const validation = validateInlineTableRows(tableEditor.parameter, parsed.rows);
    setCsvPreview({ rows: parsed.rows, error: validation || undefined });
  };
  const runEntryOperation = async (node: CanvasNode, capability: BackendCapability) => {
    if (!client || !node.capabilityId || nodeControlsLocked) return;
    if (workflowDirty) {
      const saved = await saveCurrentWorkflow();
      if (!saved) return;
    }
    const dependencies = await client.dependencies();
    const missing = dependencies.filter(
      (dependency) => dependency.available === false && dependency.required_by?.includes(capability.canonical_id),
    );
    if (missing.length > 0) {
      const names = missing.map((dependency) => `${dependency.label} ${dependency.version}`).join(', ');
      if (!window.confirm(`Install required dependencies: ${names}? This may download files from the network.`)) return;
      await client.installDependencies(
        missing.map((dependency) => dependency.id),
        true,
      );
    }
    setNodes((current) => current.map((item) => (item.id === node.id ? { ...item, executionState: 'running' } : item)));
    try {
      const result = await client.runOperation(
        project.session_id,
        node.capabilityId,
        wireParameters(capability, node.parameters || {}),
        node.id,
      );
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id
            ? { ...item, executionState: 'completed', executionMessage: JSON.stringify(result) }
            : item,
        ),
      );
      await refreshArtifacts(true);
      const cacheHit =
        result !== null && typeof result === 'object' && !Array.isArray(result) && result.cache_hit === true;
      setStatus(
        cacheHit ? `${capability.label} reused cached artifacts; execution skipped.` : `${capability.label} completed.`,
      );
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Operation failed.';
      const artifacts = await refreshArtifacts(true);
      const published = artifacts.some(
        (artifact) =>
          artifact.producer_instance === node.id &&
          artifact.status === 'published' &&
          artifact.workflow_revision === workflowRevision,
      );
      if (published) {
        setNodes((current) =>
          current.map((item) =>
            item.id === node.id
              ? { ...item, executionState: 'completed', executionMessage: `Artifacts published; ${message}` }
              : item,
          ),
        );
        setStatus(`Operation completed and artifacts were published, but the response reported an error: ${message}`);
        return;
      }
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id ? { ...item, executionState: 'failed', executionMessage: message } : item,
        ),
      );
      setStatus(message);
    }
  };
  const stopOperation = async (node: CanvasNode) => {
    if (!client || node.executionState !== 'running') return;
    try {
      await client.cancelWorkflow(project.session_id);
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id ? { ...item, executionMessage: 'Operation cancellation requested.' } : item,
        ),
      );
      setStatus('Operation cancellation requested.');
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Operation cancellation failed.');
    }
  };
  const workflowExecutionActive = ['queued', 'running', 'cancelling'].includes(workflowState);
  const runningNodeId = nodes.find((node) => node.executionState === 'running')?.id ?? null;
  const nodeExecutionActive = runningNodeId !== null;
  const nodeControlsLocked = workflowBusy || workflowExecutionActive || nodeExecutionActive;
  const selectedNode = selectedNodeId ? nodeMap.get(selectedNodeId) : undefined;
  const selectedCapability = selectedNode ? nodeCapability(selectedNode) : undefined;
  const pickerCapability = pickerCapabilityId
    ? capabilities.operations.find((item) => item.canonical_id === pickerCapabilityId)
    : undefined;
  const documentationCapability = selectedCapability || pickerCapability;
  useEffect(() => {
    if (!documentationFocus) return;
    const frame = window.requestAnimationFrame(() => {
      window.setTimeout(() => {
        const target = Array.from(document.querySelectorAll<HTMLElement>('[data-ontology-focus]')).find(
          (element) =>
            element.dataset.ontologyFocusSection === documentationFocus.section &&
            element.dataset.ontologyFocusKey === documentationFocus.key,
        );
        if (target) {
          const pane = target.closest<HTMLElement>('.sf-node-info-pane');
          if (pane) {
            pane.scrollTo({
              top: Math.max(0, target.offsetTop - pane.clientHeight / 2 + target.offsetHeight / 2),
              behavior: 'smooth',
            });
          }
          target.focus({ preventScroll: true });
        }
      }, 0);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [documentationFocus, documentationCapability]);
  const openNodeDocumentation = (nodeId: string, focus: DocumentationFocus | null = null) => {
    setDocumentationFocus(focus);
    setSelectedNodeId(nodeId);
  };
  const pathWizardNode = pathWizard ? nodes.find((node) => node.id === pathWizard.nodeId) : undefined;
  const pathWizardEntries =
    pathWizardNode && pathWizard
      ? Array.isArray(pathWizardNode.parameters?.[pathWizard.parameter.name])
        ? (pathWizardNode.parameters[pathWizard.parameter.name] as unknown[]).filter(
            (path): path is string => typeof path === 'string',
          )
        : []
      : [];
  const appendPathWizardSelection = (paths: string[]) => {
    if (!pathWizard || !pathWizardNode) return;
    const currentNodePaths = pathWizardEntries;
    if (pathWizard.mergeIntoJsonEditor && jsonEditor) {
      let current: unknown[] = currentNodePaths;
      try {
        const parsed = JSON.parse(jsonEditorText);
        if (Array.isArray(parsed)) current = parsed;
      } catch {
        current = currentNodePaths;
      }
      const merged = [...current];
      for (const path of paths) if (!merged.includes(path)) merged.push(path);
      updateNodeParameter(pathWizardNode.id, pathWizard.parameter.name, merged);
      setJsonEditorText(JSON.stringify(merged, null, 2));
      setJsonEditorError(null);
      return;
    }
    const merged = [...currentNodePaths];
    for (const path of paths) if (!merged.includes(path)) merged.push(path);
    updateNodeParameter(pathWizardNode.id, pathWizard.parameter.name, merged);
  };

  return (
    <div
      ref={canvasRef}
      className={`sf-canvas sf-canvas-shell ${gridVisible ? 'grid-visible' : ''} ${interaction?.type === 'pan' ? 'panning' : ''}`}
      data-canvas-surface={surface}
      onMouseDown={beginPan}
      onMouseUp={finishOnCanvas}
      onContextMenu={(event) => {
        if (!suppressContextMenuRef.current) return;
        event.preventDefault();
        suppressContextMenuRef.current = false;
      }}
    >
      <div className="sf-canvas-toolbar" onMouseDown={(event) => event.stopPropagation()}>
        {onProjectHub ? (
          <button
            type="button"
            className="sf-canvas-control sf-home-button"
            onClick={onProjectHub}
            aria-label="Project Hub"
            title="Project Hub"
          >
            <i className="fa-solid fa-house" />
          </button>
        ) : null}
        <button type="button" className="sf-canvas-control" onClick={() => zoom(0.1)} title="Zoom in">
          <i className="fa-solid fa-plus" />
        </button>
        <span className="sf-canvas-zoom">{Math.round(scale * 100)}%</span>
        <button type="button" className="sf-canvas-control" onClick={() => zoom(-0.1)} title="Zoom out">
          <i className="fa-solid fa-minus" />
        </button>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={arrangeNodes}
          disabled={nodeControlsLocked}
          title="Reset view and arrange nodes"
        >
          <i className="fa-solid fa-crosshairs" />
        </button>
        <button
          type="button"
          className={`sf-canvas-control ${gridVisible ? 'active' : ''}`}
          onClick={() => setGridVisible((value) => !value)}
          title="Toggle grid"
        >
          <i className="fa-solid fa-grip" />
        </button>
        <button
          type="button"
          className="sf-canvas-control"
          onClick={openStandalonePicker}
          disabled={nodeControlsLocked}
          title="Add Operation"
          aria-label="Add Operation"
        >
          <i className="fa-solid fa-diagram-project" />
        </button>
        {surface === 'workflow' && client ? (
          <>
            <input
              ref={workflowFileInputRef}
              type="file"
              accept="application/json,.json"
              onChange={(event) => void applyImportedWorkflow(event)}
              hidden
            />
            <button
              type="button"
              className={`sf-canvas-control ${workflowDirty ? 'unsaved' : ''}`}
              onClick={() => void saveCurrentWorkflow()}
              disabled={nodeControlsLocked || !workflowLoaded || !workflowDirty}
              title={workflowDirty ? 'Save workflow changes' : 'Workflow is saved'}
              aria-label="Save workflow"
            >
              <i className="fa-solid fa-floppy-disk" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={openWorkflowMetadataEditor}
              disabled={nodeControlsLocked || !workflowLoaded}
              title="Edit workflow metadata JSON"
              aria-label="Edit workflow metadata"
            >
              <i className="fa-solid fa-tag" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={discardWorkflowChanges}
              disabled={nodeControlsLocked || !workflowLoaded || !workflowDirty}
              title="Discard changes and reload saved workflow"
              aria-label="Discard workflow changes"
            >
              <i className="fa-solid fa-rotate-left" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={resetWorkflow}
              disabled={nodeControlsLocked}
              title="Reset workflow to empty"
              aria-label="Reset workflow to empty"
            >
              <i className="fa-solid fa-broom" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={undoWorkflowEdit}
              disabled={nodeControlsLocked}
              title="Undo workflow edit"
              aria-label="Undo workflow edit"
            >
              <i className="fa-solid fa-arrow-left" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={redoWorkflowEdit}
              disabled={nodeControlsLocked}
              title="Redo workflow edit"
              aria-label="Redo workflow edit"
            >
              <i className="fa-solid fa-arrow-right" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void clearArtifactCache()}
              disabled={nodeControlsLocked}
              title="Clear cached data and cache history"
              aria-label="Clear cached data and cache history"
            >
              <i className="fa-solid fa-database" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void clearAllArtifacts()}
              disabled={nodeControlsLocked}
              title="Clear all artifacts"
              aria-label="Clear all artifacts"
            >
              <i className="fa-solid fa-trash-can" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={exportWorkflow}
              disabled={nodeControlsLocked}
              title="Export workflow JSON"
              aria-label="Export workflow JSON"
            >
              <i className="fa-solid fa-file-export" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => workflowFileInputRef.current?.click()}
              disabled={nodeControlsLocked}
              title="Load workflow JSON"
              aria-label="Load workflow JSON"
            >
              <i className="fa-solid fa-file-import" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void validateCurrentWorkflow()}
              disabled={nodeControlsLocked}
              title="Validate workflow"
            >
              <i className="fa-solid fa-check" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('run')}
              disabled={nodeControlsLocked}
              title="Run workflow"
            >
              <i className="fa-solid fa-play" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('cancel')}
              disabled={!workflowExecutionActive || workflowBusy}
              title="Cancel workflow"
            >
              <i className="fa-solid fa-stop" />
            </button>
          </>
        ) : null}
      </div>
      {surface === 'workflow' && !workflowLoaded ? (
        <div className="sf-canvas-workflow-loading" role="status" aria-live="polite">
          <LoadingLogo
            size="large"
            label={
              <>
                <strong>Loading workflow</strong>
                <span>Restoring nodes and connections…</span>
              </>
            }
          />
        </div>
      ) : null}
      {surface === 'workflow' && savingWorkflow ? (
        <div className="sf-canvas-workflow-saving" role="status" aria-live="polite">
          <LoadingLogo
            label={
              <>
                <strong>Saving workflow</strong>
                <span>Updating the project definition…</span>
              </>
            }
          />
        </div>
      ) : null}
      {surface === 'workflow' && artifactCleanupBusy ? (
        <div className="sf-canvas-workflow-saving" role="status" aria-live="polite">
          <LoadingLogo
            label={
              <>
                <strong>Cleaning published output</strong>
                <span>Updating the project artifacts…</span>
              </>
            }
          />
        </div>
      ) : null}
      {surface === 'workflow' && workflowLoaded && nodes.length === 0 ? (
        <div className="sf-empty-workflow-prompt" onMouseDown={(event) => event.stopPropagation()}>
          <button type="button" onClick={openStandalonePicker} aria-label="Add operation">
            <i className="fa-solid fa-diagram-project" />
            <span>Add operation</span>
          </button>
        </div>
      ) : null}
      <div
        className="sf-canvas-status"
        role="log"
        aria-live="polite"
        aria-label="Canvas activity log"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="sf-canvas-status-heading">
          <strong>{surface === 'workflow' ? 'Workflow terminal' : 'Explorer terminal'}</strong>
        </div>
        <div className="sf-canvas-status-current">
          <span>State: {workflowState}</span>
          <span>{status}</span>
        </div>
        <div className="sf-canvas-status-feed">
          {activityLog.map((entry) => (
            <div className={`sf-canvas-status-line ${entry.level}`} key={entry.id}>
              <time>{entry.timestamp}</time>
              <span>{entry.message}</span>
            </div>
          ))}
        </div>
      </div>
      <div
        className="sf-canvas-world"
        style={{
          width: canvasWorldSize.width,
          height: canvasWorldSize.height,
          transform: `translate(${offset.x}px, ${offset.y}px) scale(${scale})`,
        }}
      >
        {selectionBox ? (
          <div
            className="sf-canvas-selection-box"
            style={{
              left: selectionBox.x,
              top: selectionBox.y,
              width: selectionBox.width,
              height: selectionBox.height,
            }}
            aria-hidden="true"
          />
        ) : null}
        <svg
          className="sf-canvas-edges"
          width={canvasWorldSize.width}
          height={canvasWorldSize.height}
          aria-hidden="true"
        >
          {edges.map((edge) => {
            const source = nodeMap.get(edge.source);
            const target = nodeMap.get(edge.target);
            if (!source || !target) return null;
            const from = positionedPort(source, edge.sourcePort, 'output');
            const to = positionedPort(target, edge.targetPort, 'input');
            const bend = Math.max(70, Math.abs(to.x - from.x) * 0.45);
            return (
              <path
                key={edge.id}
                className={`${typeClass(typedPort(source, edge.sourcePort, 'output'))}${selectedEdgeId === edge.id ? ' selected' : ''}`}
                d={`M ${from.x} ${from.y} C ${from.x + bend} ${from.y}, ${to.x - bend} ${to.y}, ${to.x} ${to.y}`}
                onMouseDown={(event) => beginEdgeReconnect(event, edge)}
                onClick={(event) => {
                  event.stopPropagation();
                  setSelectedEdgeId(edge.id);
                  setSelectedNodeId(null);
                }}
              />
            );
          })}
          {connection
            ? (() => {
                const source = nodeMap.get(connection.source);
                if (!source) return null;
                const from = positionedPort(source, connection.sourcePort, 'output');
                const bend = Math.max(70, Math.abs(connection.point.x - from.x) * 0.45);
                return (
                  <path
                    className={`pending ${typeClass(typedPort(source, connection.sourcePort, 'output'))}`}
                    d={`M ${from.x} ${from.y} C ${from.x + bend} ${from.y}, ${connection.point.x - bend} ${connection.point.y}, ${connection.point.x} ${connection.point.y}`}
                  />
                );
              })()
            : null}
        </svg>
        {nodes.map((node) => {
          const capability = node.capabilityId
            ? capabilities.operations.find((item) => item.canonical_id === node.capabilityId)
            : undefined;
          const domainClass = (capability?.domain || 'core').toLowerCase().replace(/[^a-z0-9]+/g, '-');
          const outputArtifact = (port: NodePort) =>
            currentArtifacts
              .filter(
                (artifact) =>
                  artifact.status === 'published' &&
                  artifactContractMatches(artifact.contract_id, port.semanticContract) &&
                  artifact.producer_instance === node.id,
              )
              .at(-1);
          return (
            <div
              key={node.id}
              className={`sf-canvas-node ${node.kind} sf-domain-${domainClass} ${node.executionState === 'running' ? 'running' : ''}${selectedNodeIds.includes(node.id) ? ' selected' : ''}`}
              style={{ left: node.x, top: node.y }}
              onMouseDown={(event) => beginNodeDrag(event, node)}
              onMouseUp={() => {
                setInteraction(null);
              }}
            >
              <div className="sf-canvas-node-header">
                <div className="sf-node-header-actions">
                  <button
                    type="button"
                    className="sf-node-info"
                    aria-label={`Open information for ${node.title}`}
                    title="Operation documentation"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      setDocumentationFocus(null);
                      setSelectedNodeId(node.id);
                    }}
                  >
                    <i className="fa-solid fa-circle-info" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-run"
                    aria-label={`Run ${node.title}`}
                    title="Run operation"
                    disabled={!capability || nodeControlsLocked}
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      if (capability) void runEntryOperation(node, capability);
                    }}
                  >
                    <i className="fa-solid fa-play" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-stop"
                    aria-label={`Stop ${node.title}`}
                    title="Stop operation"
                    disabled={workflowExecutionActive || runningNodeId !== node.id}
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      void stopOperation(node);
                    }}
                  >
                    <i className="fa-solid fa-stop" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-delete"
                    aria-label={`Delete ${node.title}`}
                    title="Delete node and connected connectors"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      deleteNode(node.id);
                    }}
                  >
                    <i className="fa-solid fa-trash" />
                  </button>
                  <button
                    type="button"
                    className="sf-node-info sf-node-clear-artifacts"
                    aria-label={`Delete output data for ${node.title}`}
                    title="Delete published output data"
                    disabled={nodeControlsLocked || !client}
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      requestClearNodeArtifacts(node);
                    }}
                  >
                    <i className="fa-solid fa-eraser" />
                  </button>
                </div>
                <strong className="sf-node-title">{node.title}</strong>
                {capability?.module_id ? <span>{capability.module_id}</span> : null}
              </div>
              {nodePorts(capability).inputs.length ? (
                <section className="sf-node-section sf-node-inputs">
                  <h4>Inputs</h4>
                  {nodePorts(capability).inputs.map((port) => (
                    <div className="sf-node-port-row" key={port.id}>
                      <button
                        type="button"
                        className={`sf-node-port input ${typeClass(visualPortTypeKey(port))}`}
                        data-canvas-anchor={`${node.id}|input|${port.id}`}
                        aria-label={`Connect to ${node.title} input ${port.label}`}
                        onMouseDown={(event) => event.stopPropagation()}
                        onMouseUp={(event) => finishConnection(event, node, port.id)}
                      >
                        <i className={typeIcon(visualPortTypeKey(port))} aria-hidden="true" />
                      </button>
                      <button
                        type="button"
                        className="sf-node-ontology-link"
                        onMouseDown={(event) => event.stopPropagation()}
                        onClick={(event) => {
                          event.stopPropagation();
                          openNodeDocumentation(node.id, { section: 'inputs', key: port.id });
                        }}
                      >
                        {port.label}
                        {port.required ? ' *' : ''}
                      </button>
                    </div>
                  ))}
                </section>
              ) : null}
              <section className="sf-node-section sf-node-outputs">
                <h4>Outputs</h4>
                {nodePorts(capability).outputs.length ? (
                  nodePorts(capability).outputs.map((port) => (
                    <div
                      className={`sf-node-port-row output ${outputArtifact(port) ? 'has-artifact' : ''}`}
                      key={port.id}
                    >
                      <button
                        type="button"
                        className="sf-node-ontology-link"
                        onMouseDown={(event) => event.stopPropagation()}
                        onClick={(event) => {
                          event.stopPropagation();
                          openNodeDocumentation(node.id, { section: 'outputs', key: port.id });
                        }}
                      >
                        {port.label}
                      </button>
                      <button
                        type="button"
                        className={`sf-node-port output ${typeClass(visualPortTypeKey(port))} ${outputArtifact(port) ? 'available' : ''}`}
                        data-canvas-anchor={`${node.id}|output|${port.id}`}
                        aria-label={`Connect from ${node.title} output ${port.label}`}
                        onMouseDown={(event) => beginConnection(event, node, port.id)}
                        onMouseEnter={() => showOutputRendererMenu(`${node.id}|${port.id}`)}
                        onMouseLeave={hideOutputRendererMenu}
                        onMouseUp={(event) => event.stopPropagation()}
                        onDoubleClick={(event) => {
                          event.stopPropagation();
                          pendingConnectionRef.current = null;
                          setConnection(null);
                          const artifact = outputArtifact(port);
                          if (artifact) openDefaultArtifactViewer(artifact as ArtifactRecord);
                        }}
                        onClick={(event) => {
                          if (event.detail !== 2) return;
                          event.stopPropagation();
                          pendingConnectionRef.current = null;
                          setConnection(null);
                          const artifact = outputArtifact(port);
                          if (artifact) openDefaultArtifactViewer(artifact as ArtifactRecord);
                        }}
                      >
                        <i className={typeIcon(visualPortTypeKey(port))} aria-hidden="true" />
                      </button>
                      {outputArtifact(port) && hoveredOutputAnchor === `${node.id}|${port.id}` ? (
                        <div
                          className="sf-output-artifact-popover sf-output-renderer-menu is-visible"
                          role="dialog"
                          aria-label={`Render ${port.label}`}
                          onMouseDown={(event) => event.stopPropagation()}
                          onMouseEnter={() => showOutputRendererMenu(`${node.id}|${port.id}`)}
                          onMouseLeave={hideOutputRendererMenu}
                        >
                          <strong>{port.label}</strong>
                          <span className="sf-output-artifact-id">
                            <b>Artifact ID:</b> <code>{(outputArtifact(port) as ArtifactRecord).artifact_id}</code>
                          </span>
                          <span>{artifactSummary(outputArtifact(port) as ArtifactRecord)}</span>
                          {(() => {
                            const artifact = outputArtifact(port) as ArtifactRecord;
                            const compatibleViewers = compatibleArtifactViewers({
                              semanticType: artifact.contract_id,
                              representation: artifact.representation,
                            });
                            return compatibleViewers.length || isVisualizationArtifact(artifact) ? (
                              <>
                                {compatibleViewers.map((viewer) => (
                                  <button
                                    type="button"
                                    key={viewer.id}
                                    onClick={(event) => {
                                      event.stopPropagation();
                                      openArtifactViewer(artifact, 'default', viewer.id);
                                    }}
                                  >
                                    {viewer.label}
                                  </button>
                                ))}
                                {isVisualizationArtifact(artifact) ? (
                                  <button
                                    type="button"
                                    onClick={(event) => {
                                      event.stopPropagation();
                                      openArtifactViewer(artifact, 'default');
                                    }}
                                  >
                                    View JSON
                                  </button>
                                ) : null}
                              </>
                            ) : null;
                          })()}
                        </div>
                      ) : null}
                    </div>
                  ))
                ) : (
                  <div className="sf-node-empty">No declared outputs</div>
                )}
              </section>
              {capability && capability.parameters.length > 0 ? (
                <>
                  <button
                    type="button"
                    className="sf-node-section-toggle"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={() => setExpandedParameters((current) => ({ ...current, [node.id]: !current[node.id] }))}
                  >
                    <span>Parameters</span>
                    {!(expandedParameters[node.id] ?? node.projectEntry) ? (
                      <span
                        className="sf-node-parameter-summary-port"
                        data-canvas-parameter-summary={node.id}
                        title="Parameter connectors; expand Parameters to edit individual connections"
                        aria-label="Collapsed parameter connectors"
                      >
                        <i className="fa-solid fa-sliders" aria-hidden="true" />
                      </span>
                    ) : null}
                    <i
                      className={`fa-solid fa-chevron-${(expandedParameters[node.id] ?? node.projectEntry) ? 'up' : 'down'}`}
                    />
                  </button>
                  {(expandedParameters[node.id] ?? node.projectEntry) ? (
                    <div className="sf-canvas-node-parameters" onMouseDown={(event) => event.stopPropagation()}>
                      {capability.parameters.map((parameter) => {
                        const value = node.parameters?.[parameter.name];
                        if (isTableParameter(parameter)) {
                          return (
                            <div className="sf-canvas-parameter" key={parameter.name}>
                              <button
                                type="button"
                                className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                                data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                onMouseDown={(event) => event.stopPropagation()}
                                onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                              >
                                <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                              </button>
                              <div className="sf-canvas-parameter-heading">
                                <button
                                  type="button"
                                  className="sf-node-ontology-link"
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onClick={(event) => {
                                    event.stopPropagation();
                                    openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                  }}
                                >
                                  {parameter.label || parameter.name}
                                </button>
                              </div>
                              <div className="sf-json-parameter-actions">
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => openJsonEditor(node, parameter)}
                                >
                                  <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                </button>
                              </div>
                            </div>
                          );
                        }
                        if (isFileListParameter(parameter)) {
                          const entries = Array.isArray(value) ? value : [];
                          return (
                            <div className="sf-canvas-parameter" key={parameter.name}>
                              <button
                                type="button"
                                className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                                data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                onMouseDown={(event) => event.stopPropagation()}
                                onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                              >
                                <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                              </button>
                              <div className="sf-canvas-parameter-heading">
                                <button
                                  type="button"
                                  className="sf-node-ontology-link"
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onClick={(event) => {
                                    event.stopPropagation();
                                    openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                  }}
                                >
                                  {parameter.label || parameter.name}
                                </button>
                                <span>
                                  {parameter.extensions?.length ? `.${parameter.extensions.join(', .')}` : ''}
                                  {parameter.directory_extensions?.length
                                    ? ` · folders: .${parameter.directory_extensions.join(', .')}`
                                    : ''}
                                </span>
                              </div>
                              <div className="sf-canvas-parameter-actions">
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => openJsonEditor(node, parameter)}
                                >
                                  <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                </button>
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => void addSelectedPaths(node, parameter, false)}
                                >
                                  <i className="fa-solid fa-file" /> Choose files
                                </button>
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => void addSelectedPaths(node, parameter, true)}
                                >
                                  <i className="fa-solid fa-folder" /> Choose folders
                                </button>
                              </div>
                              <small className="sf-canvas-file-summary">
                                {entries.length
                                  ? `${entries.length} path${entries.length === 1 ? '' : 's'} selected`
                                  : 'No paths selected'}
                              </small>
                              {entries.map((entry, index) => {
                                const isSimplePath = isSimplePathParameter(parameter);
                                const record =
                                  typeof entry === 'object' && entry !== null ? (entry as Record<string, unknown>) : {};
                                const pathValue = typeof entry === 'string' ? entry : String(record.path || '');
                                return (
                                  <div className="sf-canvas-file-entry" key={`${pathValue || 'new'}-${index}`}>
                                    <input
                                      aria-label={`Path ${index + 1}`}
                                      value={pathValue}
                                      placeholder="Path"
                                      onChange={(event) => {
                                        const next = entries.map((item, itemIndex) =>
                                          itemIndex === index
                                            ? isSimplePath
                                              ? event.target.value
                                              : { ...record, path: event.target.value }
                                            : item,
                                        );
                                        updateNodeParameter(node.id, parameter.name, next);
                                      }}
                                    />
                                    {!isSimplePath ? (
                                      <>
                                        <input
                                          aria-label={`Replicate ${index + 1}`}
                                          value={String(record.replicate_name || '')}
                                          placeholder="Replicate"
                                          onChange={(event) => {
                                            const next = entries.map((item, itemIndex) =>
                                              itemIndex === index
                                                ? { ...record, replicate_name: event.target.value }
                                                : item,
                                            );
                                            updateNodeParameter(node.id, parameter.name, next);
                                          }}
                                        />
                                        <input
                                          aria-label={`Blank ${index + 1}`}
                                          value={String(record.blank_name || '')}
                                          placeholder="Blank"
                                          onChange={(event) => {
                                            const next = entries.map((item, itemIndex) =>
                                              itemIndex === index
                                                ? { ...record, blank_name: event.target.value }
                                                : item,
                                            );
                                            updateNodeParameter(node.id, parameter.name, next);
                                          }}
                                        />
                                      </>
                                    ) : null}
                                    <button
                                      type="button"
                                      className="sf-button secondary"
                                      onClick={() =>
                                        updateNodeParameter(
                                          node.id,
                                          parameter.name,
                                          entries.filter((_, itemIndex) => itemIndex !== index),
                                        )
                                      }
                                    >
                                      Remove
                                    </button>
                                  </div>
                                );
                              })}
                            </div>
                          );
                        }
                        const constrainedValues = Array.isArray(parameter.schema.enum)
                          ? parameter.schema.enum.filter(
                              (candidate) =>
                                candidate === null || ['string', 'number', 'boolean'].includes(typeof candidate),
                            )
                          : [];
                        const scalarType = String(
                          Array.isArray(parameter.schema.type) ? parameter.schema.type[0] : parameter.schema.type,
                        );
                        const isDecimalParameter = ['number', 'real', 'float', 'double'].includes(scalarType);
                        return (
                          <div className="sf-canvas-parameter" key={parameter.name}>
                            <button
                              type="button"
                              className={`sf-node-parameter-port ${typeClass(parameterTypeKey(parameter))}`}
                              data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                              aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                              onMouseDown={(event) => event.stopPropagation()}
                              onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                            >
                              <i className={typeIcon(parameterTypeKey(parameter))} aria-hidden="true" />
                            </button>
                            <div className="sf-canvas-parameter-heading">
                              <button
                                type="button"
                                className="sf-node-ontology-link"
                                onMouseDown={(event) => event.stopPropagation()}
                                onClick={(event) => {
                                  event.stopPropagation();
                                  openNodeDocumentation(node.id, { section: 'parameters', key: parameter.name });
                                }}
                              >
                                {parameter.label || parameter.name}
                              </button>
                            </div>
                            {isJsonParameter(parameter) ? (
                              <div className="sf-json-parameter-actions">
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => openJsonEditor(node, parameter)}
                                >
                                  <i className={typeIcon(parameterTypeKey(parameter))} /> Edit
                                </button>
                              </div>
                            ) : (
                              <div
                                className={isSimplePathParameter(parameter) ? 'sf-canvas-scalar-path-row' : undefined}
                              >
                                {constrainedValues.length > 0 ? (
                                  <select
                                    value={
                                      constrainedValues.some(
                                        (candidate) => String(candidate) === parameterInputValue(parameter, value),
                                      )
                                        ? parameterInputValue(parameter, value)
                                        : ''
                                    }
                                    onChange={(event) =>
                                      updateNodeParameter(
                                        node.id,
                                        parameter.name,
                                        scalarInputValue(parameter, event.target.value),
                                      )
                                    }
                                  >
                                    {!parameter.required ? <option value="">Select an option</option> : null}
                                    {constrainedValues.map((candidate) => (
                                      <option key={String(candidate)} value={String(candidate)}>
                                        {String(candidate)}
                                      </option>
                                    ))}
                                  </select>
                                ) : (
                                  <input
                                    type={
                                      scalarType === 'boolean'
                                        ? 'checkbox'
                                        : isDecimalParameter
                                          ? 'text'
                                          : scalarType === 'integer'
                                            ? 'number'
                                            : 'text'
                                    }
                                    {...(scalarType === 'boolean'
                                      ? {
                                          checked: Boolean(
                                            value ?? parameter.default ?? parameter.schema.default ?? false,
                                          ),
                                          onChange: (event: ChangeEvent<HTMLInputElement>) =>
                                            updateNodeParameter(node.id, parameter.name, event.target.checked),
                                        }
                                      : {
                                          value: parameterInputValue(parameter, value),
                                          inputMode: isDecimalParameter ? 'decimal' : undefined,
                                          pattern: isDecimalParameter
                                            ? '[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?'
                                            : undefined,
                                          'aria-invalid':
                                            isDecimalParameter &&
                                            parameterInputValue(parameter, value).trim() !== '' &&
                                            !Number.isFinite(Number(parameterInputValue(parameter, value))),
                                          onChange: (event: ChangeEvent<HTMLInputElement>) =>
                                            updateNodeParameter(
                                              node.id,
                                              parameter.name,
                                              isDecimalParameter
                                                ? event.target.value
                                                : scalarInputValue(parameter, event.target.value),
                                            ),
                                          onBlur: isDecimalParameter
                                            ? (event: ChangeEvent<HTMLInputElement>) => {
                                                const raw = event.target.value.trim();
                                                if (!raw) {
                                                  updateNodeParameter(node.id, parameter.name, null);
                                                } else if (Number.isFinite(Number(raw))) {
                                                  updateNodeParameter(node.id, parameter.name, Number(raw));
                                                } else {
                                                  setStatus(
                                                    `${parameter.label || parameter.name} must be a finite number.`,
                                                  );
                                                }
                                              }
                                            : undefined,
                                        })}
                                  />
                                )}
                                {isSimplePathParameter(parameter) ? (
                                  <button
                                    type="button"
                                    className="sf-icon-button sf-canvas-path-picker"
                                    aria-label={`Choose ${parameter.label || parameter.name}`}
                                    title="Choose file or folder"
                                    onClick={() => void chooseSinglePath(node, parameter)}
                                  >
                                    <i className="fa-solid fa-folder-open" aria-hidden="true" />
                                  </button>
                                ) : null}
                              </div>
                            )}
                          </div>
                        );
                      })}
                    </div>
                  ) : null}
                </>
              ) : null}
            </div>
          );
        })}
      </div>
      {metadataEditorOpen ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setMetadataEditorOpen(false)}>
          <section className="sf-json-editor-modal" onMouseDown={(event) => event.stopPropagation()}>
            <header>
              <div>
                <h2>Workflow metadata</h2>
                <small>Edit workflow metadata or add any extra JSON entries.</small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                onClick={() => setMetadataEditorOpen(false)}
                aria-label="Close metadata editor"
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            <textarea
              value={metadataEditorText}
              onChange={(event) => {
                setMetadataEditorText(event.target.value);
                setMetadataEditorError(null);
              }}
              spellCheck={false}
              aria-label="Workflow metadata JSON"
            />
            {metadataEditorError ? <p className="sf-json-editor-error">{metadataEditorError}</p> : null}
            <footer>
              <button type="button" className="sf-button secondary" onClick={() => setMetadataEditorOpen(false)}>
                Cancel
              </button>
              <button type="button" className="sf-button primary" onClick={() => void saveWorkflowMetadata()}>
                Validate and apply
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {artifactCleanupNode ? (
        <div
          className="sf-dialog-backdrop sf-canvas-confirmation-backdrop"
          role="presentation"
          onMouseDown={() => setArtifactCleanupNode(null)}
        >
          <section
            className="sf-dialog sf-canvas-confirmation-dialog"
            role="dialog"
            aria-modal="true"
            aria-labelledby="sf-clear-artifacts-title"
            onMouseDown={(event) => event.stopPropagation()}
          >
            <div className="sf-dialog-heading">
              <div>
                <h2 id="sf-clear-artifacts-title">Delete published output data?</h2>
                <small>
                  This removes all published artifacts created by <strong>{artifactCleanupNode.title}</strong> while
                  keeping the workflow node and its parameters.
                </small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close cleanup confirmation"
                onClick={() => setArtifactCleanupNode(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </div>
            <div className="sf-dialog-actions">
              <button type="button" className="sf-button secondary" onClick={() => setArtifactCleanupNode(null)}>
                Cancel
              </button>
              <button
                type="button"
                className="sf-button primary"
                onClick={() => void clearNodeArtifacts(artifactCleanupNode)}
              >
                Delete published data
              </button>
            </div>
          </section>
        </div>
      ) : null}
      {tableEditor ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setTableEditor(null)}>
          <section
            className="sf-json-editor-modal sf-table-editor-modal"
            onMouseDown={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <h2>{tableEditor.parameter.label || tableEditor.parameter.name}</h2>
                <small>Type: {schemaTypeLabel(tableEditor.parameter.schema)}</small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close table editor"
                onClick={() => setTableEditor(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            <div className="sf-table-editor-content">
              <table>
                <thead>
                  <tr>
                    {schemaPropertyOrder(tableEditor.parameter.schema).map((columnName) => {
                      const schema = tableEditor.parameter.schema.properties?.[columnName] || {};
                      return (
                        <th key={columnName}>
                          <span>
                            {columnName}
                            {(tableEditor.parameter.schema.required || []).includes(columnName) ? ' *' : ''}
                          </span>
                          <small>{schema.type || 'value'}</small>
                          {schema.type === 'path' ? (
                            <div className="sf-table-editor-path-actions">
                              <button
                                type="button"
                                className="sf-button secondary"
                                onClick={() => void chooseTableColumnPaths(columnName, schema)}
                              >
                                <i className="fa-solid fa-file" /> Choose files
                              </button>
                              <button
                                type="button"
                                className="sf-button secondary"
                                onClick={() => void chooseTableColumnPaths(columnName, schema, true)}
                              >
                                <i className="fa-solid fa-folder" /> Choose folders
                              </button>
                            </div>
                          ) : null}
                        </th>
                      );
                    })}
                    <th aria-label="Row actions" />
                  </tr>
                </thead>
                <tbody>
                  {tableEditor.rows.map((row, rowIndex) => (
                    <tr key={rowIndex}>
                      {schemaPropertyOrder(tableEditor.parameter.schema).map((columnName) => {
                        const schema = tableEditor.parameter.schema.properties?.[columnName] || {};
                        return (
                          <td key={columnName}>
                            <input
                              aria-label={`${schema.title || columnName} row ${rowIndex + 1}`}
                              value={String(row[columnName] ?? '')}
                              onChange={(event) => updateTableCell(rowIndex, columnName, event.target.value)}
                            />
                          </td>
                        );
                      })}
                      <td>
                        <button
                          type="button"
                          className="sf-button secondary"
                          onClick={() =>
                            setTableEditor((current) =>
                              current
                                ? { ...current, rows: current.rows.filter((_, index) => index !== rowIndex) }
                                : current,
                            )
                          }
                        >
                          Remove
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {!tableEditor.rows.length ? <p className="sf-table-editor-empty">No rows added.</p> : null}
            </div>
            {tableEditor.error ? <div className="sf-json-editor-error">{tableEditor.error}</div> : null}
            {csvPreview ? (
              <div className="sf-table-csv-preview">
                <strong>CSV preview</strong>
                <span>{csvPreview.error || `${csvPreview.rows.length} rows ready to import.`}</span>
                {!csvPreview.error ? (
                  <button
                    type="button"
                    className="sf-button secondary"
                    onClick={() => {
                      setTableEditor((current) =>
                        current ? { ...current, rows: [...current.rows, ...csvPreview.rows], error: null } : current,
                      );
                      setCsvPreview(null);
                    }}
                  >
                    Accept preview
                  </button>
                ) : null}
                <button type="button" className="sf-button secondary" onClick={() => setCsvPreview(null)}>
                  Dismiss
                </button>
              </div>
            ) : null}
            <footer>
              <label className="sf-button secondary">
                Import CSV
                <input
                  type="file"
                  accept=".csv,text/csv"
                  hidden
                  onChange={(event) => {
                    const file = event.target.files?.[0];
                    if (file) void importTableCsv(file);
                    event.currentTarget.value = '';
                  }}
                />
              </label>
              <button
                type="button"
                className="sf-button secondary"
                onClick={() =>
                  setTableEditor((current) => {
                    if (!current) return current;
                    const columns = Object.keys(current.parameter.schema.properties || {});
                    return {
                      ...current,
                      rows: [
                        ...current.rows,
                        Object.fromEntries(columns.map((column) => [column, ''])) as Record<string, unknown>,
                      ],
                    };
                  })
                }
              >
                <i className="fa-solid fa-plus" /> Add row
              </button>
              <button type="button" className="sf-button secondary" onClick={() => setTableEditor(null)}>
                Cancel
              </button>
              <button type="button" className="sf-button" onClick={saveTableEditor}>
                Save table
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {jsonEditor ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setJsonEditor(null)}>
          <section className="sf-json-editor-modal" onMouseDown={(event) => event.stopPropagation()}>
            <header>
              <div>
                <h2>{jsonEditor.parameter.label || jsonEditor.parameter.name}</h2>
                <small>Type: {schemaTypeLabel(jsonEditor.parameter.schema)}</small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close JSON editor"
                onClick={() => setJsonEditor(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            <textarea
              value={jsonEditorText}
              spellCheck={false}
              aria-label="JSON value"
              onChange={(event) => {
                setJsonEditorText(event.target.value);
                setJsonEditorError(null);
              }}
            />
            {jsonEditorError ? <div className="sf-json-editor-error">{jsonEditorError}</div> : null}
            <footer>
              <button type="button" className="sf-button secondary" onClick={() => setJsonEditor(null)}>
                Cancel
              </button>
              {isPathListParameter(jsonEditor.parameter) ? (
                <button type="button" className="sf-button secondary" onClick={openPathWizardFromJsonEditor}>
                  <i className="fa-solid fa-folder-tree" /> Add files/folders
                </button>
              ) : null}
              {jsonEditor.parameter.schema.type === 'table' ? (
                <label className="sf-button secondary">
                  <i className="fa-solid fa-file-csv" /> Load CSV
                  <input
                    type="file"
                    accept=".csv,text/csv"
                    hidden
                    onChange={(event) => {
                      const file = event.target.files?.[0];
                      if (file) void importJsonCsv(file);
                      event.currentTarget.value = '';
                    }}
                  />
                </label>
              ) : null}
              {jsonEditor.parameter.schema.type === 'table' ? (
                <button type="button" className="sf-button secondary" onClick={openTableEditorFromJsonEditor}>
                  <i className="fa-solid fa-table" /> Edit as table
                </button>
              ) : null}
              <button type="button" className="sf-button" onClick={saveJsonEditor}>
                Save
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {pathWizard && pathWizardNode ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setPathWizard(null)}>
          <section
            className="sf-json-editor-modal sf-path-wizard-modal"
            onMouseDown={(event) => event.stopPropagation()}
            onWheel={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <h2>{pathWizard.mergeIntoJsonEditor ? 'Add files and folders' : 'Select files and folders'}</h2>
                <small>
                  {pathWizard.mergeIntoJsonEditor
                    ? 'Add files and vendor directories to the JSON string array.'
                    : 'Select multiple files and folders, then connect the output to a compatible array input.'}
                </small>
              </div>
              <button
                type="button"
                className="sf-icon-button"
                aria-label="Close path selector"
                onClick={() => setPathWizard(null)}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </header>
            {client ? (
              <PathFileManager
                client={client}
                selectedPaths={pathWizardEntries}
                onAddPaths={appendPathWizardSelection}
              />
            ) : null}
            <footer>
              <button type="button" className="sf-button secondary" onClick={() => setPathWizard(null)}>
                Cancel
              </button>
              <button type="button" className="sf-button" onClick={() => setPathWizard(null)}>
                Done
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {picker ? (
        <div className="sf-canvas-picker sf-operation-deck" onMouseDown={(event) => event.stopPropagation()}>
          <div className="sf-operation-deck-heading">
            <div>
              <strong>{surface === 'explorer' ? 'Choose an operation' : 'Choose the next operation'}</strong>
            </div>
            <button
              type="button"
              className="sf-icon-button sf-close-button"
              aria-label="Close operation palette"
              onClick={() => {
                setPicker(null);
                setPickerCapabilityId(null);
              }}
            >
              <i className="fa-solid fa-xmark" />
            </button>
          </div>
          <div className="sf-operation-deck-search">
            <i className="fa-solid fa-magnifying-glass" aria-hidden="true" />
            <input
              aria-label="Search operation ontology"
              placeholder="Search ontology (regex)"
              value={paletteSearch}
              onChange={(event) => setPaletteSearch(event.target.value)}
            />
            <select
              aria-label="Filter operations by module"
              value={paletteModule}
              onChange={(event) => setPaletteModule(event.target.value)}
            >
              <option value="">All modules</option>
              {paletteModules.map((module) => (
                <option value={module} key={module}>
                  {module}
                </option>
              ))}
            </select>
          </div>
          <div className="sf-operation-deck-list">
            {filteredTemplates.map((template) => {
              const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
              const ports = nodePorts(capability);
              const metadata = `${template.module || 'core'} · ${ports.inputs.length} input${ports.inputs.length === 1 ? '' : 's'} · ${ports.outputs.length} output${ports.outputs.length === 1 ? '' : 's'}`;
              return (
                <article
                  className={`sf-operation-deck-card sf-operation-deck-card-${operationDeckDensity(template.title, metadata)} sf-domain-${(
                    template.domain || 'unknown'
                  )
                    .replace(/[^a-z0-9]+/gi, '-')
                    .toLowerCase()}`}
                  key={template.id}
                  data-domain={template.domain || 'unknown'}
                >
                  <div className="sf-operation-deck-card-content">
                    <strong>{template.title}</strong>
                    <small>{metadata}</small>
                  </div>
                  <button
                    type="button"
                    className="sf-icon-button"
                    aria-label={`Read about ${template.title}`}
                    onClick={() => setPickerCapabilityId(template.capabilityId || null)}
                  >
                    <i className="fa-solid fa-circle-info" />
                  </button>
                  <button type="button" className="sf-button" onClick={() => addNode(template)}>
                    Add
                  </button>
                </article>
              );
            })}
          </div>
          {!filteredTemplates.length ? (
            <div className="sf-operation-deck-empty">No compatible capability is available for this connection.</div>
          ) : null}
        </div>
      ) : null}
      {documentationCapability ? (
        <div
          className="sf-side-pane-backdrop sf-node-info-backdrop"
          onMouseDown={(event) => {
            if (event.target === event.currentTarget) {
              setDocumentationFocus(null);
              setSelectedNodeId(null);
              setPickerCapabilityId(null);
            }
          }}
        >
          <aside className="sf-side-pane sf-node-info-pane" onMouseDown={(event) => event.stopPropagation()}>
            <div className="sf-node-info-heading">
              <div>
                <h2>{documentationCapability.label}</h2>
              </div>
              <button
                type="button"
                className="sf-icon-button sf-close-button"
                aria-label="Close operation documentation"
                onClick={() => {
                  setDocumentationFocus(null);
                  setSelectedNodeId(null);
                  setPickerCapabilityId(null);
                }}
              >
                <i className="fa-solid fa-xmark" />
              </button>
            </div>
            <p>{documentationCapability.definition}</p>
            {documentationCapability.interface.guidance ? (
              <section className="sf-node-info-guidance">
                <h3>Guidance</h3>
                <p>{documentationCapability.interface.guidance}</p>
              </section>
            ) : null}
            <section>
              <h3>Inputs</h3>
              {nodePorts(documentationCapability).inputs.map((port) => (
                <div
                  className="sf-info-row"
                  key={port.id}
                  data-ontology-focus="true"
                  data-ontology-focus-section="inputs"
                  data-ontology-focus-key={port.id}
                  tabIndex={-1}
                >
                  {ontologyPortTerm(port) && onOpenOntologyWiki ? (
                    <button
                      type="button"
                      className="sf-ontology-term-link"
                      onClick={() => onOpenOntologyWiki(ontologyPortTerm(port))}
                    >
                      {port.label}
                    </button>
                  ) : (
                    <strong>{port.label}</strong>
                  )}
                  <span>
                    {port.required ? 'required' : 'optional'}
                    {port.description ? (
                      <>
                        <br />
                        <small>{port.description}</small>
                      </>
                    ) : null}
                    {portOntologyDetails(port).map((detail) => (
                      <Fragment key={detail}>
                        <br />
                        <small>{detail}</small>
                      </Fragment>
                    ))}
                  </span>
                </div>
              ))}
            </section>
            <section>
              <h3>Outputs</h3>
              {nodePorts(documentationCapability).outputs.map((port) => (
                <div
                  className="sf-info-row"
                  key={port.id}
                  data-ontology-focus="true"
                  data-ontology-focus-section="outputs"
                  data-ontology-focus-key={port.id}
                  tabIndex={-1}
                >
                  {ontologyPortTerm(port) && onOpenOntologyWiki ? (
                    <button
                      type="button"
                      className="sf-ontology-term-link"
                      onClick={() => onOpenOntologyWiki(ontologyPortTerm(port))}
                    >
                      {port.label}
                    </button>
                  ) : (
                    <strong>{port.label}</strong>
                  )}
                  <span>
                    {port.description || 'value'}
                    {portOntologyDetails(port).map((detail) => (
                      <Fragment key={detail}>
                        <br />
                        <small>{detail}</small>
                      </Fragment>
                    ))}
                  </span>
                </div>
              ))}
            </section>
            <section>
              <h3>Parameters</h3>
              {documentationCapability.parameters.map((parameter) => (
                <div
                  className="sf-info-row"
                  key={parameter.name}
                  data-ontology-focus="true"
                  data-ontology-focus-section="parameters"
                  data-ontology-focus-key={parameter.name}
                  tabIndex={-1}
                >
                  {onOpenOntologyWiki ? (
                    <button
                      type="button"
                      className="sf-ontology-term-link"
                      onClick={() => onOpenOntologyWiki(parameter.name)}
                    >
                      {parameter.label || parameter.name}
                    </button>
                  ) : (
                    <strong>{parameter.label || parameter.name}</strong>
                  )}
                  <span>
                    {parameter.description || parameter.schema.type || 'value'}
                    <br />
                    <small>type: {schemaTypeLabel(parameter.schema)}</small>
                    {parameterOntologyDetails(parameter).map((detail) => (
                      <Fragment key={detail}>
                        <br />
                        <small>{detail}</small>
                      </Fragment>
                    ))}
                    {parameter.default !== undefined || parameter.schema.default !== undefined ? (
                      <>
                        <br />
                        <small>Default: {JSON.stringify(parameter.default ?? parameter.schema.default)}</small>
                      </>
                    ) : null}
                    {Array.isArray(parameter.schema.examples) && parameter.schema.examples.length ? (
                      <>
                        <br />
                        <small>
                          Examples:{' '}
                          {JSON.stringify(
                            parameter.schema.examples.length === 1
                              ? parameter.schema.examples[0]
                              : parameter.schema.examples,
                          )}
                        </small>
                      </>
                    ) : null}
                  </span>
                </div>
              ))}
            </section>
          </aside>
        </div>
      ) : null}
      {artifactViewer ? (
        <ArtifactViewerHost
          artifact={artifactViewer}
          sessionId={project.session_id}
          viewerId={artifactViewerId ?? undefined}
          mode={artifactViewerMode}
          pluginApi={pluginApi}
          variant={
            artifactViewerId !== null || isVisualizationArtifact(artifactViewer)
              ? 'visualization'
              : artifactViewer.representation === 'table' || artifactViewerMode === 'table'
                ? 'wide'
                : 'json'
          }
          onClose={() => setArtifactViewer(null)}
          fallback={<pre className="sf-artifact-viewer-json">{prettyArtifactPayload(artifactViewer.payload)}</pre>}
        />
      ) : null}
    </div>
  );
}
