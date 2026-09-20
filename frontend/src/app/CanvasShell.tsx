import { useEffect, useMemo, useRef, useState, type MouseEvent as ReactMouseEvent } from 'react';
import { StreamFindApiClient, type ProjectSession } from '../backend/StreamFindApiClient';
import type {
  BackendCapability,
  CapabilityParameter,
  CapabilityPort,
  JsonSchema,
  ServiceCapabilities,
  WorkflowState,
} from '../backend/protocol';

type CanvasNodeKind = 'project' | 'operation' | 'method' | 'extract' | 'plot';
type CanvasNode = {
  id: string;
  kind: CanvasNodeKind;
  title: string;
  description: string;
  x: number;
  y: number;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
  parameters?: Record<string, unknown>;
  executionState?: 'idle' | 'running' | 'completed' | 'failed';
  executionMessage?: string;
};
type CanvasEdge = { id: string; source: string; sourcePort: string; target: string; targetPort: string };
type Point = { x: number; y: number };
type Interaction = {
  type: 'node' | 'pan';
  id?: string;
  offsetX?: number;
  offsetY?: number;
  originX?: number;
  originY?: number;
};
type ConnectionDraft = { source: string; sourcePort: string; point: Point };

type NodeTemplate = {
  id: string;
  kind: Exclude<CanvasNodeKind, 'project'>;
  title: string;
  description: string;
  icon: string;
  outputsToCanvas: boolean;
  capabilityId?: string;
  projectEntry?: boolean;
};

function capabilityTemplates(capabilities: ServiceCapabilities, projectDomain: string): NodeTemplate[] {
  const backend = capabilities.operations
    .filter(
      (capability) =>
        (capability.domain === projectDomain || (projectDomain === 'core' && capability.domain === 'streamfind')) &&
        capability.kind === 'operation',
    )
    .map((capability: BackendCapability) => ({
      id: capability.canonical_id,
      kind: capability.kind,
      title: capability.label,
      description: capability.definition,
      icon: capability.kind === 'operation' ? 'fa-solid fa-bolt' : 'fa-solid fa-gears',
      outputsToCanvas: capability.kind === 'operation',
      capabilityId: capability.canonical_id,
      projectEntry: capability.project_entry === true,
    }));
  return backend;
}

function defaultParameters(capability: BackendCapability, project: ProjectSession): Record<string, unknown> {
  return Object.fromEntries(
    capability.parameters
      .filter((parameter) => parameter.name !== 'database_path')
      .map((parameter) => [
        parameter.name,
        parameter.default ?? (parameter.schema.type === 'array' || parameter.schema.type === 'table' ? [] : ''),
      ])
      .concat([['database_path', project.database_path]]),
  );
}

function isFileListParameter(parameter: CapabilityParameter): boolean {
  if (parameter.schema.type !== 'array' || parameter.schema.items?.type !== 'object') return false;
  if (parameter.extensions?.length || parameter.path_kind) return true;
  const properties = parameter.schema.items.properties;
  return Boolean(
    properties &&
    Object.values(properties).some(
      (property) => property.type === 'path' || property.format === 'path' || property.format === 'file-path',
    ),
  );
}

function isTableParameter(parameter: CapabilityParameter): boolean {
  return parameter.schema.type === 'table' && Boolean(parameter.schema.properties);
}

function isJsonParameter(parameter: CapabilityParameter): boolean {
  return parameter.schema.type === 'object' || (parameter.schema.type === 'array' && !isFileListParameter(parameter));
}

type NodePort = {
  id: string;
  label: string;
  required?: boolean;
  description?: string;
  semanticContract?: string;
  dataKind?: CapabilityPort['data_kind'];
  schema?: JsonSchema;
  representations?: string[];
  typeKey: string;
};

function portTypeKey(port: CapabilityPort | NodePort): string {
  const dataKind = 'dataKind' in port ? port.dataKind : (port as CapabilityPort).data_kind;
  const contract = 'semanticContract' in port ? port.semanticContract : (port as CapabilityPort).semantic_contract;
  if (dataKind && contract) return `${dataKind}:${contract}`;
  if (dataKind) return dataKind;
  if (contract) return contract;
  const representations = 'representations' in port ? port.representations : undefined;
  if (representations?.length) return representations.slice().sort().join('|');
  return 'unknown';
}

function schemaShape(schema: JsonSchema | undefined): string {
  if (!schema) return '';
  const normalized: Record<string, unknown> = {};
  for (const key of ['type', 'properties', 'items', 'required', 'additionalProperties', 'enum']) {
    if (schema[key] !== undefined) normalized[key] = schema[key];
  }
  if (normalized.properties && typeof normalized.properties === 'object') {
    normalized.properties = Object.fromEntries(
      Object.entries(normalized.properties as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, value]) => {
          const property = value as JsonSchema;
          const columnSchema =
            normalized.type === 'table' && property.type === 'array' && property.items ? property.items : property;
          return [key, schemaShape(columnSchema)];
        }),
    );
  }
  if (normalized.items && typeof normalized.items === 'object') {
    normalized.items = schemaShape(normalized.items as JsonSchema);
  }
  return JSON.stringify(normalized);
}

function typeIcon(typeKey: string): string {
  const type = typeKey.toLowerCase();
  if (type.includes('duckdb_table')) return 'fa-solid fa-database';
  if (type.includes('tabular_value')) return 'fa-solid fa-table';
  if (type.includes('table') || type.includes('result')) return 'fa-solid fa-database';
  if (type.includes('json') || type.includes('object') || type.includes('array')) return 'fa-solid fa-code';
  if (type.includes('path') || type.includes('file')) return 'fa-solid fa-file';
  if (type.includes('bool')) return 'fa-solid fa-toggle-on';
  if (type.includes('int') || type.includes('real') || type.includes('float') || type.includes('double'))
    return 'fa-solid fa-hashtag';
  if (type.includes('string') || type.includes('text') || type.includes('varchar')) return 'fa-solid fa-font';
  return 'fa-solid fa-circle-nodes';
}

function typeClass(typeKey: string): string {
  const type = typeKey.toLowerCase();
  if (type.includes('duckdb_table')) return 'sf-port-type-duckdb-table';
  if (type.includes('tabular_value')) return 'sf-port-type-tabular-value';
  if (type.includes('table') || type.includes('result')) return 'sf-port-type-table';
  if (type.includes('json') || type.includes('object') || type.includes('array')) return 'sf-port-type-json';
  if (type.includes('path') || type.includes('file')) return 'sf-port-type-path';
  if (type.includes('bool')) return 'sf-port-type-boolean';
  if (type.includes('int') || type.includes('real') || type.includes('float') || type.includes('double'))
    return 'sf-port-type-number';
  if (type.includes('string') || type.includes('text') || type.includes('varchar')) return 'sf-port-type-string';
  return 'sf-port-type-unknown';
}

function nodePorts(capability: BackendCapability | undefined): { inputs: NodePort[]; outputs: NodePort[] } {
  const canvas = capability?.canvas;
  const inputPorts = canvas?.input_ports || capability?.input_ports || capability?.inputs || [];
  const outputPorts = capability?.output_ports || capability?.outputs || [];
  const inputs = inputPorts.map((port: CapabilityPort) => ({
    id: port.id,
    label: port.label || port.id,
    required: port.required ?? port.optional !== true,
    description: port.schema?.description,
    semanticContract: port.semantic_contract,
    dataKind: port.data_kind,
    schema: port.schema,
    representations: port.representations,
    typeKey: portTypeKey(port),
  }));
  const outputs = outputPorts.length
    ? outputPorts.map((port: CapabilityPort) => ({
        id: port.id,
        label: port.label || port.id,
        description: port.schema?.description,
        semanticContract: port.semantic_contract,
        dataKind: port.data_kind,
        schema: port.schema,
        representations: port.representations,
        typeKey: portTypeKey(port),
      }))
    : (canvas && 'output_results' in canvas ? canvas.output_results : []).map((result) => ({
        id: result.canonical_id,
        label: result.label,
        description: result.definition,
        typeKey: 'unknown',
      }));
  return { inputs, outputs };
}

const NODE_WIDTH = 224;
const NODE_HEIGHT = 260;
const CANVAS_WORLD_WIDTH = 6000;
const CANVAS_WORLD_HEIGHT = 4000;
const GRID_SIZE = 22;

export default function CanvasShell({
  project,
  capabilities,
  surface,
  client,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  surface: 'workflow' | 'explorer';
  client?: StreamFindApiClient;
}) {
  const canvasRef = useRef<HTMLDivElement | null>(null);
  const nextId = useRef(1);
  const [nodes, setNodes] = useState<CanvasNode[]>(() => {
    const entryTemplates = capabilityTemplates(capabilities, project.domain).filter(
      (template) => template.projectEntry,
    );
    return entryTemplates.map((template, index) => {
      const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      return {
        id: `entry-${template.id}`,
        kind: template.kind,
        title: template.title,
        description: template.description,
        x: 2800,
        y: 1800 + index * 260,
        outputsToCanvas: false,
        capabilityId: template.capabilityId,
        projectEntry: true,
        parameters: capability ? defaultParameters(capability, project) : {},
        executionState: 'idle' as const,
      };
    });
  });
  const [edges, setEdges] = useState<CanvasEdge[]>([]);
  const [scale, setScale] = useState(1);
  const [offset, setOffset] = useState<Point>({ x: 0, y: 0 });
  const viewportRef = useRef({ offset, scale });
  useEffect(() => {
    viewportRef.current = { offset, scale };
  }, [offset, scale]);
  const [interaction, setInteraction] = useState<Interaction | null>(null);
  const [connection, setConnection] = useState<ConnectionDraft | null>(null);
  const [picker, setPicker] = useState<(Point & { source?: string; sourcePort?: string }) | null>(null);
  const [pickerCapabilityId, setPickerCapabilityId] = useState<string | null>(null);
  const [jsonEditor, setJsonEditor] = useState<{ nodeId: string; parameter: CapabilityParameter } | null>(null);
  const [tableEditor, setTableEditor] = useState<{
    nodeId: string;
    parameter: CapabilityParameter;
    rows: Record<string, unknown>[];
  } | null>(null);
  const [jsonEditorText, setJsonEditorText] = useState('');
  const [jsonEditorError, setJsonEditorError] = useState<string | null>(null);
  const [gridVisible, setGridVisible] = useState(false);
  const [workflowState, setWorkflowState] = useState<WorkflowState>('idle');
  const [workflowProgress, setWorkflowProgress] = useState({ completed: 0, total: 0, current_step: 0 });
  const [workflowBusy, setWorkflowBusy] = useState(false);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [selectedEdgeId, setSelectedEdgeId] = useState<string | null>(null);
  const [paletteSearch, setPaletteSearch] = useState('');
  const [anchorCenters, setAnchorCenters] = useState<Record<string, Point>>({});
  const [expandedParameters, setExpandedParameters] = useState<Record<string, boolean>>({});
  const [status, setStatus] = useState(
    surface === 'workflow' ? 'Drag an output connector to a compatible input connector.' : 'Workflow canvas',
  );
  const nodeTemplates = useMemo(
    () => capabilityTemplates(capabilities, project.domain),
    [capabilities, project.domain],
  );

  useEffect(() => {
    if (surface !== 'workflow' || !client) return undefined;
    let active = true;
    client
      .workflowState(project.session_id)
      .then((result) => {
        if (active) {
          setWorkflowState(result.state);
          if (result.progress) setWorkflowProgress(result.progress);
        }
      })
      .catch(() => undefined);
    return () => {
      active = false;
    };
  }, [client, project.session_id, surface]);

  const workflowAction = async (action: 'validate' | 'run' | 'pause' | 'cancel') => {
    if (!client) return;
    setWorkflowBusy(true);
    try {
      const result =
        action === 'validate'
          ? await client.validateWorkflow(project.session_id)
          : action === 'run'
            ? await client.runWorkflow(project.session_id)
            : action === 'pause'
              ? await client.pauseWorkflow(project.session_id)
              : await client.cancelWorkflow(project.session_id);
      setWorkflowState(result.state);
      if (result.progress) setWorkflowProgress(result.progress);
      setStatus(`Workflow ${action} request accepted.`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : `Workflow ${action} failed.`);
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
  const portPosition = (node: CanvasNode, portId: string, direction: 'input' | 'output'): Point => {
    const measured = anchorCenters[`${node.id}|${direction}|${portId}`];
    if (measured) return measured;
    const ports = nodePorts(nodeCapability(node))[direction === 'input' ? 'inputs' : 'outputs'];
    const capability = nodeCapability(node);
    if (direction === 'input' && portId.startsWith('parameter:')) {
      const parameterIndex = (capability?.parameters || [])
        .filter((parameter) => parameter.name !== 'database_path')
        .findIndex((parameter) => `parameter:${parameter.name}` === portId);
      const inputCount = ports.length;
      const outputCount = Math.max(1, nodePorts(capability).outputs.length);
      const parametersTop = 58 + (inputCount ? 25 + inputCount * 31 : 0) + 25 + outputCount * 31 + 54;
      const expanded = expandedParameters[node.id] ?? node.projectEntry;
      return {
        x: node.x - 12,
        y: node.y + parametersTop + (expanded ? Math.max(0, parameterIndex) * 38 + 8 : 15),
      };
    }
    const index = Math.max(
      0,
      ports.findIndex((port) => port.id === portId),
    );
    const top =
      direction === 'input'
        ? 94 + index * 31
        : (ports.length && nodePorts(capability).inputs.length ? 120 + nodePorts(capability).inputs.length * 31 : 94) +
          index * 31;
    return { x: node.x + (direction === 'input' ? -12 : NODE_WIDTH + 12), y: node.y + top };
  };

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
        capability.parameters
          .filter((parameter) => parameter.name !== 'database_path')
          .forEach((parameter) => {
            next[`${node.id}|input|parameter:${parameter.name}`] = point;
          });
      });
      setAnchorCenters(next);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [capabilities.operations, expandedParameters, nodeMap, nodes, offset, scale]);

  useEffect(() => {
    const move = (event: globalThis.MouseEvent) => {
      if (interaction?.type === 'pan') {
        setOffset({ x: event.clientX - (interaction.originX || 0), y: event.clientY - (interaction.originY || 0) });
        return;
      }
      if (interaction?.type === 'node' && interaction.id) {
        const rect = canvasRef.current?.getBoundingClientRect();
        if (!rect) return;
        let x = (event.clientX - rect.left - offset.x) / scale - (interaction.offsetX || 0);
        let y = (event.clientY - rect.top - offset.y) / scale - (interaction.offsetY || 0);
        if (gridVisible) {
          x = Math.round(x / GRID_SIZE) * GRID_SIZE;
          y = Math.round(y / GRID_SIZE) * GRID_SIZE;
        }
        setNodes((current) =>
          current.map((node) =>
            node.id === interaction.id ? { ...node, x: Math.max(12, x), y: Math.max(12, y) } : node,
          ),
        );
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
    const up = () => setInteraction(null);
    const blur = () => {
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
  }, [connection, gridVisible, interaction, offset, scale]);

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
      if ((event.target as HTMLElement).closest('.sf-canvas-toolbar, .sf-canvas-picker, .sf-node-info-pane')) return;
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
    if (
      (event.button !== 0 && event.button !== 1) ||
      target.closest('.sf-canvas-node, .sf-canvas-toolbar, .sf-canvas-picker')
    )
      return;
    setPicker(null);
    setInteraction({ type: 'pan', originX: event.clientX - offset.x, originY: event.clientY - offset.y });
  };
  const beginNodeDrag = (event: ReactMouseEvent, node: CanvasNode) => {
    event.stopPropagation();
    const point = toWorld(event);
    setPicker(null);
    setInteraction({ type: 'node', id: node.id, offsetX: point.x - node.x, offsetY: point.y - node.y });
  };
  const beginConnection = (event: ReactMouseEvent, node: CanvasNode, sourcePort: string) => {
    event.stopPropagation();
    setPicker(null);
    setConnection({ source: node.id, sourcePort, point: portPosition(node, sourcePort, 'output') });
    setStatus('Drag this output to a compatible input on another operation.');
  };
  const canConnect = (source: CanvasNode, target: CanvasNode, targetPort: string): boolean => {
    const sourcePorts = nodePorts(nodeCapability(source)).outputs;
    const targetPorts = nodePorts(nodeCapability(target)).inputs;
    const targetCapability = nodeCapability(target);
    const parameterName = targetPort.startsWith('parameter:') ? targetPort.slice('parameter:'.length) : '';
    const sourcePort = sourcePorts.find((port) => port.id === connection?.sourcePort);
    const targetInput = targetPorts.find((port) => port.id === targetPort);
    const targetParameter = targetCapability?.parameters.find((parameter) => parameter.name === parameterName);
    const compatibleTypes =
      sourcePort && targetInput
        ? sourcePort.typeKey !== 'unknown' &&
          targetInput.typeKey !== 'unknown' &&
          sourcePort.typeKey === targetInput.typeKey
        : false;
    const parameterCompatible =
      sourcePort && targetParameter
        ? schemaShape(sourcePort.schema) === schemaShape(targetParameter.schema) &&
          sourcePort.dataKind ===
            (targetParameter.schema.type === 'table'
              ? 'tabular_value'
              : targetParameter.schema.type === 'object' || targetParameter.schema.type === 'array'
                ? 'structured_value'
                : targetParameter.schema.type)
        : false;
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
        point: portPosition(source, edge.sourcePort, 'output'),
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
      } else if (selectedNodeId) {
        event.preventDefault();
        setNodes((current) => current.filter((node) => node.id !== selectedNodeId));
        setEdges((current) =>
          current.filter((edge) => edge.source !== selectedNodeId && edge.target !== selectedNodeId),
        );
        setSelectedNodeId(null);
        setStatus('Deleted node and its connected connectors.');
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [selectedEdgeId, selectedNodeId]);
  const addNode = (template: NodeTemplate) => {
    if (!picker) return;
    const id = `${template.kind}-${nextId.current++}`;
    const node: CanvasNode = {
      id,
      kind: template.kind,
      title: template.title,
      description: template.description,
      x: picker.x - NODE_WIDTH / 2,
      y: picker.y - NODE_HEIGHT / 2,
      outputsToCanvas: template.outputsToCanvas,
      capabilityId: template.capabilityId,
      projectEntry: template.projectEntry,
      parameters: template.capabilityId
        ? defaultParameters(
            capabilities.operations.find((item) => item.canonical_id === template.capabilityId) as BackendCapability,
            project,
          )
        : {},
    };
    setNodes((current) => [...current, node]);
    const source = picker.source;
    if (source) {
      const targetCapability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      const sourceNode = nodeMap.get(source);
      const sourcePort =
        sourceNode && nodePorts(nodeCapability(sourceNode)).outputs.find((port) => port.id === picker.sourcePort);
      const targetPort =
        nodePorts(targetCapability).inputs.find((port) => sourcePort && port.typeKey === sourcePort.typeKey)?.id || '';
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
          Boolean(sourcePort && nodePorts(targetCapability).inputs.some((port) => port.typeKey === sourcePort.typeKey))
        );
      return false;
    });
  }, [capabilities.operations, nodeMap, nodeTemplates, picker]);
  const filteredTemplates = useMemo(() => {
    if (!paletteSearch.trim()) return availableTemplates;
    let expression: RegExp;
    try {
      expression = new RegExp(paletteSearch, 'i');
    } catch {
      return [];
    }
    return availableTemplates.filter((template) => {
      const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
      return expression.test(JSON.stringify(capability || template));
    });
  }, [availableTemplates, capabilities.operations, paletteSearch]);
  const arrangeNodes = () => {
    if (!nodes.length) return;
    const indegree = new Map(nodes.map((node) => [node.id, 0]));
    const outgoing = new Map<string, string[]>();
    edges.forEach((edge) => {
      indegree.set(edge.target, (indegree.get(edge.target) || 0) + 1);
      outgoing.set(edge.source, [...(outgoing.get(edge.source) || []), edge.target]);
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
    Array.from(groups.keys())
      .sort((a, b) => a - b)
      .forEach((stage) => {
        const column = groups.get(stage) || [];
        column.sort((a, b) => a.y - b.y || a.id.localeCompare(b.id));
        column.forEach((node, row) => {
          const target = byId.get(node.id);
          if (!target) return;
          target.x = 2600 + stage * 360;
          target.y = 2000 + (row - (column.length - 1) / 2) * 190;
        });
      });
    setNodes(arranged);
    setScale(1);
    const minX = Math.min(...arranged.map((node) => node.x));
    const minY = Math.min(...arranged.map((node) => node.y));
    const maxX = Math.max(...arranged.map((node) => node.x + NODE_WIDTH));
    const maxY = Math.max(...arranged.map((node) => node.y + NODE_HEIGHT));
    const canvas = canvasRef.current;
    if (canvas)
      setOffset({ x: canvas.clientWidth / 2 - (minX + maxX) / 2, y: canvas.clientHeight / 2 - (minY + maxY) / 2 });
    setStatus('Arranged nodes by dependency stage from left to right.');
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
    setJsonEditorText(JSON.stringify(node.parameters?.[parameter.name] ?? {}, null, 2));
    setJsonEditorError(null);
  };
  const saveJsonEditor = () => {
    if (!jsonEditor) return;
    try {
      updateNodeParameter(jsonEditor.nodeId, jsonEditor.parameter.name, JSON.parse(jsonEditorText));
      setJsonEditor(null);
      setJsonEditorError(null);
    } catch (error) {
      setJsonEditorError(error instanceof Error ? error.message : 'Invalid JSON.');
    }
  };
  const addSelectedPaths = async (node: CanvasNode, parameter: CapabilityParameter, folders: boolean) => {
    if (!client) return;
    try {
      const paths = folders ? await client.pickFolders() : await client.pickPaths(parameter.extensions || []);
      const current: unknown[] = Array.isArray(node.parameters?.[parameter.name])
        ? (node.parameters[parameter.name] as unknown[])
        : [];
      const existing = new Set(
        current.map((item) =>
          typeof item === 'object' && item !== null ? String((item as Record<string, unknown>).path || '') : '',
        ),
      );
      const added = paths
        .filter((path) => !existing.has(path))
        .map((path) => ({ path, replicate_name: '', blank_name: '' }));
      updateNodeParameter(node.id, parameter.name, [...current, ...added]);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Path selection failed.');
    }
  };
  const openTableEditor = (node: CanvasNode, parameter: CapabilityParameter) => {
    const value = node.parameters?.[parameter.name];
    const rows = Array.isArray(value)
      ? value.filter((row): row is Record<string, unknown> => typeof row === 'object' && row !== null)
      : [];
    setTableEditor({ nodeId: node.id, parameter, rows });
  };
  const updateTableCell = (rowIndex: number, columnName: string, value: string) => {
    setTableEditor((current) => {
      if (!current) return current;
      const rows = current.rows.map((row, index) => (index === rowIndex ? { ...row, [columnName]: value } : row));
      return { ...current, rows };
    });
  };
  const chooseTableColumnPaths = async (columnName: string, schema: JsonSchema) => {
    if (!client || !tableEditor) return;
    const declared = schema.extensions;
    const extensions = Array.isArray(declared)
      ? declared.filter((item): item is string => typeof item === 'string')
      : [];
    try {
      const paths = await client.pickPaths(extensions);
      const columns = Object.keys(tableEditor.parameter.schema.properties || {});
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
    updateNodeParameter(tableEditor.nodeId, tableEditor.parameter.name, tableEditor.rows);
    setTableEditor(null);
  };
  const runEntryOperation = async (node: CanvasNode, capability: BackendCapability) => {
    if (!client || !node.capabilityId) return;
    setNodes((current) => current.map((item) => (item.id === node.id ? { ...item, executionState: 'running' } : item)));
    try {
      const result = await client.runOperation(project.session_id, node.capabilityId, node.parameters || {});
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id
            ? { ...item, executionState: 'completed', executionMessage: JSON.stringify(result) }
            : item,
        ),
      );
      setStatus(`${capability.label} completed.`);
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Operation failed.';
      setNodes((current) =>
        current.map((item) =>
          item.id === node.id ? { ...item, executionState: 'failed', executionMessage: message } : item,
        ),
      );
      setStatus(message);
    }
  };
  const selectedNode = selectedNodeId ? nodeMap.get(selectedNodeId) : undefined;
  const selectedCapability = selectedNode ? nodeCapability(selectedNode) : undefined;
  const pickerCapability = pickerCapabilityId
    ? capabilities.operations.find((item) => item.canonical_id === pickerCapabilityId)
    : undefined;
  const documentationCapability = selectedCapability || pickerCapability;

  return (
    <div
      ref={canvasRef}
      className={`sf-canvas sf-canvas-shell ${gridVisible ? 'grid-visible' : ''} ${interaction?.type === 'pan' ? 'panning' : ''}`}
      data-canvas-surface={surface}
      onMouseDown={beginPan}
      onMouseUp={finishOnCanvas}
    >
      <div className="sf-canvas-toolbar" onMouseDown={(event) => event.stopPropagation()}>
        <button type="button" className="sf-canvas-control" onClick={() => zoom(0.1)} title="Zoom in">
          <i className="fa-solid fa-plus" />
        </button>
        <span className="sf-canvas-zoom">{Math.round(scale * 100)}%</span>
        <button type="button" className="sf-canvas-control" onClick={() => zoom(-0.1)} title="Zoom out">
          <i className="fa-solid fa-minus" />
        </button>
        <button type="button" className="sf-canvas-control" onClick={arrangeNodes} title="Reset view and arrange nodes">
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
          title="Add Operation"
          aria-label="Add Operation"
        >
          <i className="fa-solid fa-diagram-project" />
        </button>
        {surface === 'workflow' && client ? (
          <>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('validate')}
              disabled={workflowBusy}
              title="Validate workflow"
            >
              <i className="fa-solid fa-check" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('run')}
              disabled={workflowBusy}
              title="Run workflow"
            >
              <i className="fa-solid fa-play" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('pause')}
              disabled={workflowBusy}
              title="Pause workflow"
            >
              <i className="fa-solid fa-pause" />
            </button>
            <button
              type="button"
              className="sf-canvas-control"
              onClick={() => void workflowAction('cancel')}
              disabled={workflowBusy}
              title="Cancel workflow"
            >
              <i className="fa-solid fa-stop" />
            </button>
          </>
        ) : null}
      </div>
      <div className="sf-canvas-status">
        <strong>{surface === 'workflow' ? 'Workflow canvas' : 'Explorer canvas'}</strong>
        {surface === 'workflow'
          ? ` · ${workflowState} · ${workflowProgress.completed}/${workflowProgress.total}`
          : null}{' '}
        · {status}
      </div>
      <div className="sf-canvas-world" style={{ transform: `translate(${offset.x}px, ${offset.y}px) scale(${scale})` }}>
        <svg className="sf-canvas-edges" width={CANVAS_WORLD_WIDTH} height={CANVAS_WORLD_HEIGHT} aria-hidden="true">
          {edges.map((edge) => {
            const source = nodeMap.get(edge.source);
            const target = nodeMap.get(edge.target);
            if (!source || !target) return null;
            const from = portPosition(source, edge.sourcePort, 'output');
            const to = portPosition(target, edge.targetPort, 'input');
            const bend = Math.max(70, Math.abs(to.x - from.x) * 0.45);
            return (
              <path
                key={edge.id}
                className={selectedEdgeId === edge.id ? 'selected' : undefined}
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
                const from = portPosition(source, connection.sourcePort, 'output');
                const bend = Math.max(70, Math.abs(connection.point.x - from.x) * 0.45);
                return (
                  <path
                    className="pending"
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
          return (
            <div
              key={node.id}
              className={`sf-canvas-node ${node.kind}`}
              style={{ left: node.x, top: node.y }}
              onMouseDown={(event) => beginNodeDrag(event, node)}
              onMouseUp={() => {
                setInteraction(null);
              }}
            >
              <div className="sf-canvas-node-header">
                <strong>{node.title}</strong>
                <div className="sf-node-header-actions">
                  <button
                    type="button"
                    className="sf-node-info"
                    aria-label={`Open information for ${node.title}`}
                    title="Operation documentation"
                    onMouseDown={(event) => event.stopPropagation()}
                    onClick={(event) => {
                      event.stopPropagation();
                      setSelectedNodeId(node.id);
                    }}
                  >
                    <i className="fa-solid fa-circle-info" />
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
                </div>
              </div>
              {nodePorts(capability).inputs.length ? (
                <section className="sf-node-section sf-node-inputs">
                  <h4>Inputs</h4>
                  {nodePorts(capability).inputs.map((port) => (
                    <div className="sf-node-port-row" key={port.id} title={port.description}>
                      <button
                        type="button"
                        className={`sf-node-port input ${typeClass(port.typeKey)}`}
                        data-canvas-anchor={`${node.id}|input|${port.id}`}
                        aria-label={`Connect to ${node.title} input ${port.label}`}
                        onMouseDown={(event) => event.stopPropagation()}
                        onMouseUp={(event) => finishConnection(event, node, port.id)}
                      >
                        <i className={typeIcon(port.typeKey)} aria-hidden="true" />
                      </button>
                      <span>
                        {port.label}
                        {port.required ? ' *' : ''}
                      </span>
                    </div>
                  ))}
                </section>
              ) : null}
              <section className="sf-node-section sf-node-outputs">
                <h4>Outputs</h4>
                {nodePorts(capability).outputs.length ? (
                  nodePorts(capability).outputs.map((port) => (
                    <div className="sf-node-port-row output" key={port.id} title={port.description}>
                      <span>{port.label}</span>
                      <button
                        type="button"
                        className={`sf-node-port output ${typeClass(port.typeKey)}`}
                        data-canvas-anchor={`${node.id}|output|${port.id}`}
                        aria-label={`Connect from ${node.title} output ${port.label}`}
                        onMouseDown={(event) => beginConnection(event, node, port.id)}
                      >
                        <i className={typeIcon(port.typeKey)} aria-hidden="true" />
                      </button>
                    </div>
                  ))
                ) : (
                  <div className="sf-node-empty">No declared outputs</div>
                )}
              </section>
              {capability && capability.parameters.some((parameter) => parameter.name !== 'database_path') ? (
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
                      {capability.parameters
                        .filter((parameter) => parameter.name !== 'database_path')
                        .map((parameter) => {
                          const value = node.parameters?.[parameter.name];
                          if (isTableParameter(parameter)) {
                            const rows = Array.isArray(value) ? value.length : 0;
                            return (
                              <div className="sf-canvas-parameter" key={parameter.name}>
                                <button
                                  type="button"
                                  className={`sf-node-parameter-port ${typeClass('tabular_value')}`}
                                  data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                  aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                                >
                                  <i className={typeIcon('tabular_value')} aria-hidden="true" />
                                </button>
                                <div className="sf-canvas-parameter-heading">
                                  <strong>{parameter.label || parameter.name}</strong>
                                </div>
                                <div className="sf-canvas-parameter-actions">
                                  <button
                                    type="button"
                                    className="sf-button secondary sf-table-editor-trigger"
                                    onClick={() => openTableEditor(node, parameter)}
                                  >
                                    <i className="fa-solid fa-table" /> Edit table
                                  </button>
                                </div>
                                <small className="sf-canvas-file-summary">
                                  {rows} row{rows === 1 ? '' : 's'}
                                </small>
                              </div>
                            );
                          }
                          if (isFileListParameter(parameter)) {
                            const entries = Array.isArray(value) ? value : [];
                            return (
                              <div className="sf-canvas-parameter" key={parameter.name}>
                                <button
                                  type="button"
                                  className={`sf-node-parameter-port ${typeClass(String(parameter.schema.type || 'value'))}`}
                                  data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                  aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                  onMouseDown={(event) => event.stopPropagation()}
                                  onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                                >
                                  <i
                                    className={typeIcon(String(parameter.schema.type || 'value'))}
                                    aria-hidden="true"
                                  />
                                </button>
                                <div className="sf-canvas-parameter-heading">
                                  <strong>{parameter.label || parameter.name}</strong>
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
                                    onClick={() => void addSelectedPaths(node, parameter, false)}
                                  >
                                    <i className="fa-solid fa-folder-open" /> Choose files or .d folder
                                  </button>
                                </div>
                                <small className="sf-canvas-file-summary">
                                  {entries.length
                                    ? `${entries.length} path${entries.length === 1 ? '' : 's'} selected`
                                    : 'No paths selected'}
                                </small>
                                {entries.map((entry, index) => {
                                  const record =
                                    typeof entry === 'object' && entry !== null
                                      ? (entry as Record<string, unknown>)
                                      : {};
                                  return (
                                    <div
                                      className="sf-canvas-file-entry"
                                      key={`${String(record.path || 'new')}-${index}`}
                                    >
                                      <input
                                        aria-label={`Path ${index + 1}`}
                                        value={String(record.path || '')}
                                        placeholder="Path"
                                        onChange={(event) => {
                                          const next = entries.map((item, itemIndex) =>
                                            itemIndex === index ? { ...record, path: event.target.value } : item,
                                          );
                                          updateNodeParameter(node.id, parameter.name, next);
                                        }}
                                      />
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
                                            itemIndex === index ? { ...record, blank_name: event.target.value } : item,
                                          );
                                          updateNodeParameter(node.id, parameter.name, next);
                                        }}
                                      />
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
                          return (
                            <div className="sf-canvas-parameter" key={parameter.name}>
                              <button
                                type="button"
                                className={`sf-node-parameter-port ${typeClass(String(parameter.schema.type || 'value'))}`}
                                data-canvas-anchor={`${node.id}|input|parameter:${parameter.name}`}
                                aria-label={`Connect to ${node.title} parameter ${parameter.label || parameter.name}`}
                                onMouseDown={(event) => event.stopPropagation()}
                                onMouseUp={(event) => finishConnection(event, node, `parameter:${parameter.name}`)}
                              >
                                <i className={typeIcon(String(parameter.schema.type || 'value'))} aria-hidden="true" />
                              </button>
                              <div className="sf-canvas-parameter-heading">
                                <strong>{parameter.label || parameter.name}</strong>
                              </div>
                              {isJsonParameter(parameter) ? (
                                <button
                                  type="button"
                                  className="sf-button secondary"
                                  onClick={() => openJsonEditor(node, parameter)}
                                >
                                  <i className="fa-solid fa-code" /> Edit JSON
                                </button>
                              ) : (
                                <input
                                  value={String(value ?? parameter.default ?? parameter.schema.default ?? '')}
                                  onChange={(event) => updateNodeParameter(node.id, parameter.name, event.target.value)}
                                />
                              )}
                            </div>
                          );
                        })}
                      <button
                        type="button"
                        className="sf-button"
                        disabled={node.executionState === 'running'}
                        onClick={() => void runEntryOperation(node, capability)}
                      >
                        <i className="fa-solid fa-play" />{' '}
                        {node.executionState === 'running' ? 'Running…' : 'Run operation'}
                      </button>
                      {node.executionMessage ? (
                        <small className={`sf-operation-state ${node.executionState}`}>{node.executionMessage}</small>
                      ) : null}
                    </div>
                  ) : null}
                </>
              ) : null}
            </div>
          );
        })}
      </div>
      {tableEditor ? (
        <div className="sf-json-editor-overlay" onMouseDown={() => setTableEditor(null)}>
          <section
            className="sf-json-editor-modal sf-table-editor-modal"
            onMouseDown={(event) => event.stopPropagation()}
          >
            <header>
              <div>
                <span className="sf-eyebrow">Parameter editor</span>
                <h2>{tableEditor.parameter.label || tableEditor.parameter.name}</h2>
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
                    {(Object.entries(tableEditor.parameter.schema.properties || {}) as [string, JsonSchema][]).map(
                      ([columnName, schema]) => (
                        <th key={columnName}>
                          <span>{schema.title || columnName}</span>
                          <small>{schema.type || 'value'}</small>
                          {schema.type === 'path' ? (
                            <button
                              type="button"
                              className="sf-button secondary"
                              onClick={() => void chooseTableColumnPaths(columnName, schema)}
                            >
                              <i className="fa-solid fa-folder-open" /> Choose files
                            </button>
                          ) : null}
                        </th>
                      ),
                    )}
                    <th aria-label="Row actions" />
                  </tr>
                </thead>
                <tbody>
                  {tableEditor.rows.map((row, rowIndex) => (
                    <tr key={rowIndex}>
                      {(Object.entries(tableEditor.parameter.schema.properties || {}) as [string, JsonSchema][]).map(
                        ([columnName, schema]) => (
                          <td key={columnName}>
                            <input
                              aria-label={`${schema.title || columnName} row ${rowIndex + 1}`}
                              value={String(row[columnName] ?? '')}
                              onChange={(event) => updateTableCell(rowIndex, columnName, event.target.value)}
                            />
                          </td>
                        ),
                      )}
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
            <footer>
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
                <span className="sf-eyebrow">Parameter editor</span>
                <h2>{jsonEditor.parameter.label || jsonEditor.parameter.name}</h2>
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
              <button type="button" className="sf-button" onClick={saveJsonEditor}>
                Save JSON
              </button>
            </footer>
          </section>
        </div>
      ) : null}
      {picker ? (
        <div className="sf-canvas-picker sf-operation-deck" onMouseDown={(event) => event.stopPropagation()}>
          <div className="sf-operation-deck-heading">
            <div>
              <span className="sf-eyebrow">Workflow palette</span>
              <strong>{surface === 'explorer' ? 'Choose an operation' : 'Choose the next operation'}</strong>
              <small>Browse the available operations, inspect their ontology, or add one to the chain.</small>
            </div>
            <button
              type="button"
              className="sf-icon-button"
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
          </div>
          <div className="sf-operation-deck-list">
            {filteredTemplates.map((template) => {
              const capability = capabilities.operations.find((item) => item.canonical_id === template.capabilityId);
              const ports = nodePorts(capability);
              return (
                <article className="sf-operation-deck-card" key={template.id}>
                  <div className="sf-operation-deck-card-content">
                    <strong>{template.title}</strong>
                    <small>
                      {ports.inputs.length} input{ports.inputs.length === 1 ? '' : 's'} · {ports.outputs.length} output
                      {ports.outputs.length === 1 ? '' : 's'}
                    </small>
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
        <aside className="sf-node-info-pane" onMouseDown={(event) => event.stopPropagation()}>
          <div className="sf-node-info-heading">
            <div>
              <span className="sf-eyebrow">Ontology documentation</span>
              <h2>{documentationCapability.label}</h2>
            </div>
            <button
              type="button"
              className="sf-icon-button"
              aria-label="Close operation documentation"
              onClick={() => {
                setSelectedNodeId(null);
                setPickerCapabilityId(null);
              }}
            >
              <i className="fa-solid fa-xmark" />
            </button>
          </div>
          <p>{documentationCapability.definition}</p>
          {documentationCapability.interface_guidance ? (
            <p className="sf-node-info-note">{documentationCapability.interface_guidance}</p>
          ) : null}
          <section>
            <h3>Inputs</h3>
            {nodePorts(documentationCapability).inputs.map((port) => (
              <div className="sf-info-row" key={port.id}>
                <strong>{port.label}</strong>
                <span>{port.required ? 'required' : 'optional'}</span>
              </div>
            ))}
          </section>
          <section>
            <h3>Outputs</h3>
            {nodePorts(documentationCapability).outputs.map((port) => (
              <div className="sf-info-row" key={port.id}>
                <strong>{port.label}</strong>
                <span>{port.description}</span>
              </div>
            ))}
          </section>
          <section>
            <h3>Parameters</h3>
            {documentationCapability.parameters
              .filter((parameter) => parameter.name !== 'database_path')
              .map((parameter) => (
                <div className="sf-info-row" key={parameter.name}>
                  <strong>{parameter.label || parameter.name}</strong>
                  <span>
                    {parameter.description || parameter.schema.type || 'value'}
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
      ) : null}
    </div>
  );
}
