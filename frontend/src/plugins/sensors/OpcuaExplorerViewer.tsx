import { useEffect, useMemo, useRef, useState } from 'react';
import * as d3 from 'd3';
import type { ViewerComponentProps } from '../../framework/viewers/viewerTypes';

type OpcuaNode = {
  node_id?: string;
  browse_name?: string;
  display_name?: string;
  node_class?: number;
  node_class_name?: string;
  children?: OpcuaNode[];
};

type ExplorationPayload = {
  endpoint?: string;
  endpoint_reachable?: boolean;
  session_created?: boolean;
  detail_operation?: string;
  namespaces?: Array<{ index?: number; uri?: string }>;
  node_tree?: OpcuaNode;
};

type OpcuaReference = {
  node_id?: string;
  browse_name?: string;
  display_name?: string;
  node_class?: number;
  value?: unknown;
  value_status?: { status_code?: string; status_name?: string };
};

type OpcuaValueSnapshot = {
  available?: boolean;
  reason?: string;
  value?: unknown;
  value_status?: { status_code?: string; status_name?: string };
  read_at?: string;
  source_timestamp?: string | null;
  server_timestamp?: string | null;
};

type OpcuaNodeDetails = {
  node_id?: string;
  browse_name?: string;
  display_name?: string;
  description?: string;
  node_class_name?: string;
  data_type?: string;
  value_rank?: number;
  access_level?: number;
  user_access_level?: number;
  minimum_sampling_interval_ms?: number;
  historizing?: boolean;
  value_snapshot?: OpcuaValueSnapshot;
  type_definitions?: OpcuaReference[];
  properties?: OpcuaReference[];
};

type GraphNode = d3.SimulationNodeDatum & {
  id: string;
  label: string;
  browseName: string;
  nodeClass: number;
  nodeClassName: string;
  depth: number;
  childCount: number;
};

type GraphLink = d3.SimulationLinkDatum<GraphNode> & {
  source: string | GraphNode;
  target: string | GraphNode;
};

type GraphData = {
  nodes: GraphNode[];
  links: GraphLink[];
};

export function OpcuaExplorerViewer({ context }: ViewerComponentProps) {
  const payload = useMemo(() => readPayload(context.artifact?.payload), [context.artifact?.payload]);
  const graph = useMemo(() => (payload?.node_tree ? buildGraph(payload.node_tree) : { nodes: [], links: [] }), [payload]);
  const [query, setQuery] = useState('');
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [fitRequest, setFitRequest] = useState(0);
  const [detailsState, setDetailsState] = useState<{
    nodeId: string | null;
    loading: boolean;
    details: OpcuaNodeDetails | null;
    error: string | null;
  }>({ nodeId: null, loading: false, details: null, error: null });

  useEffect(() => {
    setSelectedId(graph.nodes[0]?.id ?? null);
    setFitRequest((request) => request + 1);
  }, [graph]);

  const selectedNode = graph.nodes.find((node) => node.id === selectedId) ?? null;
  const normalizedQuery = query.trim().toLowerCase();
  const matchingNodes = normalizedQuery
    ? graph.nodes.filter((node) => `${node.label} ${node.browseName} ${node.id}`.toLowerCase().includes(normalizedQuery))
    : [];
  const firstMatchId = matchingNodes[0]?.id ?? null;

  useEffect(() => {
    if (normalizedQuery && firstMatchId) setSelectedId(firstMatchId);
  }, [firstMatchId, normalizedQuery]);

  useEffect(() => {
    const nodeId = selectedNode?.id ?? null;
    const endpoint = payload?.endpoint;
    const client = context.pluginApi?.client;
    if (!nodeId || !endpoint || !client) {
      setDetailsState({ nodeId, loading: false, details: null, error: null });
      return undefined;
    }
    let active = true;
    setDetailsState({ nodeId, loading: true, details: null, error: null });
    void client
      .runOperation(
        context.sessionId,
        payload.detail_operation || 'sensors.inspect_opcua_node',
        { endpoint, node_id: nodeId, timeout_seconds: 10 },
        `opcua-details-${nodeId}`,
      )
      .then((value) => {
        if (!active) return;
        const details = readNodeDetails(value);
        setDetailsState({
          nodeId,
          loading: false,
          details,
          error: details ? null : 'The OPC UA node definition response was invalid.',
        });
      })
      .catch((error: unknown) => {
        if (!active) return;
        setDetailsState({
          nodeId,
          loading: false,
          details: null,
          error: error instanceof Error ? error.message : 'The OPC UA node definition could not be read.',
        });
      });
    return () => {
      active = false;
    };
  }, [context.pluginApi, context.sessionId, payload, selectedNode?.id]);

  if (!payload) return <div className="sf-opcua-explorer-state">The OPC UA exploration payload is unavailable.</div>;
  if (!payload.node_tree) {
    return (
      <div className="sf-opcua-explorer-state">
        <strong>The server responded, but no Objects node tree was returned.</strong>
        <pre>{JSON.stringify(payload, null, 2)}</pre>
      </div>
    );
  }

  return (
    <main className="sf-opcua-explorer" aria-label="OPC UA network explorer">
      <header className="sf-opcua-explorer-summary">
        <div>
          <span className="sf-opcua-explorer-eyebrow">OPC UA endpoint</span>
          <strong>{payload.endpoint || 'Unknown endpoint'}</strong>
        </div>
        <div className="sf-opcua-explorer-health" aria-live="polite">
          <span className={payload.endpoint_reachable ? 'is-good' : 'is-bad'}>
            {payload.endpoint_reachable ? 'Reachable' : 'Unavailable'}
          </span>
          <span>{payload.session_created ? 'Session created' : 'No session'}</span>
          <span>{payload.namespaces?.length ?? 0} namespaces</span>
        </div>
      </header>
      <div className="sf-opcua-explorer-toolbar">
        <label className="sf-opcua-search">
          <span>Search nodes</span>
          <input
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="Name or node ID"
            aria-label="Search OPC UA nodes"
          />
        </label>
        <div className="sf-opcua-graph-toolbar-actions">
          <span aria-live="polite">
            {normalizedQuery ? `${matchingNodes.length} matches · ` : ''}{graph.nodes.length} nodes · {graph.links.length} links
          </span>
          <button type="button" className="sf-opcua-fit" onClick={() => setFitRequest((request) => request + 1)}>
            Fit graph
          </button>
        </div>
      </div>
      <section className="sf-opcua-explorer-content">
        <div className="sf-opcua-graph-panel">
          <OpcuaGraph
            data={graph}
            query={normalizedQuery}
            selectedId={selectedId}
            fitRequest={fitRequest}
            onSelect={setSelectedId}
          />
          <div className="sf-opcua-graph-legend" aria-label="Node legend">
            <span><i className="sf-opcua-legend-dot is-object" /> Object</span>
            <span><i className="sf-opcua-legend-dot is-variable" /> Variable</span>
            <span><i className="sf-opcua-legend-dot is-other" /> Method / other</span>
            <span>Drag nodes · scroll to zoom · click to inspect</span>
          </div>
        </div>
        <aside className="sf-opcua-explorer-details" aria-label="OPC UA node details">
          <section className="sf-opcua-node-details" aria-live="polite">
            <h3>Selected node</h3>
            {selectedNode ? (
              <>
                <strong>{selectedNode.label}</strong>
                <code>{selectedNode.id}</code>
                <dl>
                  <div><dt>Browse name</dt><dd>{selectedNode.browseName || '—'}</dd></div>
                  <div><dt>Class</dt><dd>{selectedNode.nodeClassName}</dd></div>
                  <div><dt>Depth</dt><dd>{selectedNode.depth}</dd></div>
                  <div><dt>Children</dt><dd>{selectedNode.childCount}</dd></div>
                </dl>
                <NodeDefinition state={detailsState} />
              </>
            ) : <p>Select a node in the graph.</p>}
          </section>
          <section className="sf-opcua-explorer-namespaces" aria-label="OPC UA namespaces">
            <h3>Namespaces</h3>
            {payload.namespaces?.length ? (
              <ul>
                {payload.namespaces.map((namespace) => (
                  <li key={`${namespace.index}-${namespace.uri}`}>
                    <code>ns={namespace.index ?? '?'}</code>
                    <span>{namespace.uri || 'Unknown URI'}</span>
                  </li>
                ))}
              </ul>
            ) : <p>No namespace metadata returned.</p>}
          </section>
        </aside>
      </section>
    </main>
  );
}

function NodeDefinition({
  state,
}: {
  state: { nodeId: string | null; loading: boolean; details: OpcuaNodeDetails | null; error: string | null };
}) {
  if (state.loading) return <p className="sf-opcua-node-definition-state">Loading node definition…</p>;
  if (state.error) return <p className="sf-opcua-node-definition-state is-error" role="alert">{state.error}</p>;
  if (!state.details || state.details.node_id !== state.nodeId)
    return <p className="sf-opcua-node-definition-state">Node definition unavailable.</p>;
  const details = state.details;
  return (
    <section className="sf-opcua-node-definition" aria-label="OPC UA node definition">
      <h4>Definition</h4>
      <dl>
        <div><dt>Description</dt><dd>{details.description || '—'}</dd></div>
        <div><dt>Data type</dt><dd><code>{details.data_type || '—'}</code></dd></div>
        <div><dt>Value rank</dt><dd>{formatDetail(details.value_rank)}</dd></div>
        <div><dt>Access level</dt><dd>{formatDetail(details.access_level)}</dd></div>
        <div><dt>User access</dt><dd>{formatDetail(details.user_access_level)}</dd></div>
        <div><dt>Sampling interval</dt><dd>{details.minimum_sampling_interval_ms === undefined ? '—' : `${details.minimum_sampling_interval_ms} ms`}</dd></div>
        <div><dt>Historizing</dt><dd>{details.historizing === undefined ? '—' : details.historizing ? 'Yes' : 'No'}</dd></div>
      </dl>
      <ValueSnapshot snapshot={details.value_snapshot} />
      <ReferenceList title="Type definition" references={details.type_definitions} />
      <ReferenceList title="Properties" references={details.properties} />
    </section>
  );
}

function ValueSnapshot({ snapshot }: { snapshot?: OpcuaValueSnapshot }) {
  if (!snapshot) return null;
  if (snapshot.available === false) {
    return (
      <section className="sf-opcua-value-snapshot" aria-label="OPC UA value snapshot">
        <h5>Value snapshot</h5>
        <p className="sf-opcua-node-definition-state">Not queried: {snapshot.reason || 'this node has no Value attribute.'}</p>
      </section>
    );
  }
  return (
    <section className="sf-opcua-value-snapshot" aria-label="OPC UA value snapshot">
      <h5>Value snapshot</h5>
      <dl>
        <div><dt>Value</dt><dd><code>{formatDetail(snapshot.value)}</code></dd></div>
        <div><dt>Status</dt><dd>{formatStatus(snapshot.value_status)}</dd></div>
        <div><dt>Read at</dt><dd>{formatDetail(snapshot.read_at)}</dd></div>
        <div><dt>Source timestamp</dt><dd>{formatDetail(snapshot.source_timestamp)}</dd></div>
        <div><dt>Server timestamp</dt><dd>{formatDetail(snapshot.server_timestamp)}</dd></div>
      </dl>
    </section>
  );
}

function ReferenceList({ title, references }: { title: string; references?: OpcuaReference[] }) {
  if (!references?.length) return null;
  return (
    <section className="sf-opcua-node-references" aria-label={title}>
      <h5>{title}</h5>
      <ul>
        {references.map((reference) => (
          <li key={reference.node_id}>
            <strong>{reference.display_name || reference.browse_name || reference.node_id}</strong>
            <code>{reference.node_id || '—'}</code>
            {reference.value !== undefined ? <span>{formatDetail(reference.value)}</span> : null}
          </li>
        ))}
      </ul>
    </section>
  );
}

function formatDetail(value: unknown): string {
  if (value === undefined || value === null || value === '') return '—';
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'boolean') return String(value);
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function formatStatus(status?: { status_code?: string; status_name?: string }): string {
  if (!status) return '—';
  return [status.status_name, status.status_code].filter(Boolean).join(' · ') || '—';
}

function OpcuaGraph({
  data,
  query,
  selectedId,
  fitRequest,
  onSelect,
}: {
  data: GraphData;
  query: string;
  selectedId: string | null;
  fitRequest: number;
  onSelect: (nodeId: string | null) => void;
}) {
  const containerRef = useRef<HTMLDivElement>(null);
  const svgRef = useRef<SVGSVGElement>(null);
  const [size, setSize] = useState({ width: 800, height: 560 });

  useEffect(() => {
    const element = containerRef.current;
    if (!element) return undefined;
    const updateSize = () => setSize({
      width: Math.max(320, element.clientWidth),
      height: Math.max(360, element.clientHeight),
    });
    updateSize();
    const observer = new ResizeObserver(updateSize);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    const svgElement = svgRef.current;
    if (!svgElement || !data.nodes.length) return undefined;
    const svg = d3.select<SVGSVGElement, unknown>(svgElement);
    svg.selectAll('*').remove();
    svg.attr('viewBox', `0 0 ${size.width} ${size.height}`);

    const layer = svg.append('g').attr('class', 'sf-opcua-graph-layer');
    const links = layer.append('g').attr('class', 'sf-opcua-graph-links')
      .selectAll<SVGLineElement, GraphLink>('line')
      .data(data.links)
      .join('line');
    const nodes = layer.append('g').attr('class', 'sf-opcua-graph-nodes')
      .selectAll<SVGGElement, GraphNode>('g')
      .data(data.nodes, (node) => node.id)
      .join('g');
    const nodeById = new Map(data.nodes.map((node) => [node.id, node]));
    const matches = (node: GraphNode) => !query || `${node.label} ${node.browseName} ${node.id}`.toLowerCase().includes(query);
    const isRoot = (node: GraphNode) => node.depth === 0;

    nodes.classed('is-selected', (node) => node.id === selectedId)
      .classed('is-match', (node) => Boolean(query) && matches(node))
      .classed('is-dimmed', (node) => Boolean(query) && !matches(node))
      .append('circle')
      .attr('r', (node) => (isRoot(node) ? 10 : 7))
      .attr('class', (node) => nodeClassClass(node.nodeClass));
    nodes.append('title').text((node) => `${node.label} · ${node.id}`);
    nodes.append('text')
      .attr('class', 'sf-opcua-graph-label')
      .attr('dx', 11)
      .attr('dy', '0.35em')
      .text((node) => (isRoot(node) || node.depth <= 1 || node.id === selectedId || matches(node) ? node.label : ''));

    const zoom = d3.zoom<SVGSVGElement, unknown>().scaleExtent([0.2, 4]).on('zoom', (event) => {
      layer.attr('transform', event.transform);
    });
    svg.call(zoom);
    const updatePositions = () => {
      links
        .attr('x1', (link) => linkPosition(link.source, nodeById).x)
        .attr('y1', (link) => linkPosition(link.source, nodeById).y)
        .attr('x2', (link) => linkPosition(link.target, nodeById).x)
        .attr('y2', (link) => linkPosition(link.target, nodeById).y);
      nodes.attr('transform', (node) => `translate(${node.x ?? size.width / 2},${node.y ?? size.height / 2})`);
    };
    const simulation = d3.forceSimulation<GraphNode>(data.nodes)
      .force('link', d3.forceLink<GraphNode, GraphLink>(data.links).id((node) => node.id).distance(82).strength(0.8))
      .force('charge', d3.forceManyBody<GraphNode>().strength(-130))
      .force('center', d3.forceCenter(size.width / 2, size.height / 2))
      .force('collide', d3.forceCollide<GraphNode>().radius(15))
      .alphaDecay(0.08)
      .on('tick', updatePositions);
    svg.on('click', () => {
      simulation.stop();
      onSelect(null);
    });
    const drag = d3.drag<SVGGElement, GraphNode>()
      .on('start', (event, node) => {
        event.sourceEvent.stopPropagation();
        if (!event.active) simulation.alphaTarget(0.25).restart();
        node.fx = node.x;
        node.fy = node.y;
      })
      .on('drag', (event, node) => {
        node.x = event.x;
        node.y = event.y;
        node.fx = event.x;
        node.fy = event.y;
        updatePositions();
      })
      .on('end', (event, node) => {
        if (!event.active) simulation.alphaTarget(0);
        node.fx = null;
        node.fy = null;
      });
    nodes.call(drag).on('click', (event, node) => {
      event.stopPropagation();
      simulation.stop();
      onSelect(node.id);
    });

    const fit = () => {
      const focusIds = new Set(data.nodes.filter((node) => !query || matches(node)).map((node) => node.id));
      if (query) {
        for (const link of data.links) {
          const sourceId = linkNodeId(link.source);
          const targetId = linkNodeId(link.target);
          if (focusIds.has(sourceId)) focusIds.add(targetId);
          if (focusIds.has(targetId)) focusIds.add(sourceId);
        }
      }
      const focusNodes = data.nodes.filter((node) => focusIds.has(node.id));
      if (!focusNodes.length) return;
      const xs = focusNodes.map((node) => node.x ?? size.width / 2);
      const ys = focusNodes.map((node) => node.y ?? size.height / 2);
      const bounds = {
        x: Math.min(...xs) - 24,
        y: Math.min(...ys) - 24,
        width: Math.max(48, Math.max(...xs) - Math.min(...xs) + 48),
        height: Math.max(48, Math.max(...ys) - Math.min(...ys) + 48),
      };
      const padding = 48;
      const scale = Math.min((size.width - padding) / bounds.width, (size.height - padding) / bounds.height, 1.4);
      const transform = d3.zoomIdentity
        .translate(size.width / 2 - scale * (bounds.x + bounds.width / 2), size.height / 2 - scale * (bounds.y + bounds.height / 2))
        .scale(scale);
      svg.call(zoom.transform, transform);
    };
    simulation.stop();
    simulation.tick(240);
    updatePositions();
    fit();
    return () => { simulation.stop(); };
  }, [data, fitRequest, onSelect, query, size]);

  useEffect(() => {
    const svgElement = svgRef.current;
    if (!svgElement) return;
    const nodeSelection = d3.select(svgElement)
      .selectAll<SVGGElement, GraphNode>('.sf-opcua-graph-nodes > g');
    const matches = (node: GraphNode) => !query || `${node.label} ${node.browseName} ${node.id}`.toLowerCase().includes(query);
    nodeSelection
      .classed('is-selected', (node) => node.id === selectedId)
      .classed('is-match', (node) => Boolean(query) && matches(node))
      .classed('is-dimmed', (node) => Boolean(query) && !matches(node))
      .select('text')
      .text((node) => (node.depth === 0 || node.depth <= 1 || node.id === selectedId || matches(node) ? node.label : ''));
  }, [query, selectedId]);

  return (
    <div ref={containerRef} className="sf-opcua-graph" aria-label={`Interactive OPC UA network graph with ${data.nodes.length} nodes`}>
      <svg ref={svgRef} role="img" aria-label="Interactive OPC UA network graph" />
      <p className="sf-opcua-graph-fallback">Select a node to inspect its OPC UA identity.</p>
    </div>
  );
}

function buildGraph(root: OpcuaNode): GraphData {
  const nodes = new Map<string, GraphNode>();
  const links = new Map<string, GraphLink>();
  const visit = (node: OpcuaNode, depth: number, parentId?: string) => {
    const id = node.node_id || `${parentId || 'root'}:${node.browse_name || node.display_name || 'node'}`;
    const current = nodes.get(id);
    if (!current) {
      nodes.set(id, {
        id,
        label: node.display_name || node.browse_name || id,
        browseName: node.browse_name || '',
        nodeClass: node.node_class || 0,
        nodeClassName: node.node_class_name || nodeClassLabel(node.node_class || 0),
        depth,
        childCount: Array.isArray(node.children) ? node.children.length : 0,
      });
    } else {
      current.depth = Math.min(current.depth, depth);
      current.childCount = Math.max(current.childCount, Array.isArray(node.children) ? node.children.length : 0);
      if (node.node_class_name) current.nodeClassName = node.node_class_name;
    }
    if (parentId && parentId !== id) links.set(`${parentId}->${id}`, { source: parentId, target: id });
    if (!Array.isArray(node.children)) return;
    for (const child of node.children) visit(child, depth + 1, id);
  };
  visit(root, 0);
  return { nodes: Array.from(nodes.values()), links: Array.from(links.values()) };
}

function linkPosition(value: string | GraphNode, nodes: Map<string, GraphNode>) {
  const node = typeof value === 'string' ? nodes.get(value) : value;
  return { x: node?.x ?? 0, y: node?.y ?? 0 };
}

function linkNodeId(value: string | GraphNode) {
  return typeof value === 'string' ? value : value.id;
}

function nodeClassLabel(nodeClass: number) {
  if (nodeClass === 1) return 'Object';
  if (nodeClass === 2) return 'Variable';
  if (nodeClass === 4) return 'Method';
  return nodeClass ? `Class ${nodeClass}` : 'Unspecified';
}

function nodeClassClass(nodeClass: number) {
  if (nodeClass === 1) return 'is-object';
  if (nodeClass === 2) return 'is-variable';
  return 'is-other';
}

function readPayload(value: unknown): ExplorationPayload | null {
  if (typeof value === 'string') {
    try { return readPayload(JSON.parse(value)); } catch { return null; }
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  return value as ExplorationPayload;
}

function readNodeDetails(value: unknown): OpcuaNodeDetails | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const candidate = value as { node?: unknown };
  const node = candidate.node;
  if (!node || typeof node !== 'object' || Array.isArray(node)) return null;
  return node as OpcuaNodeDetails;
}
