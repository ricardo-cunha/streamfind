import { Component, type ErrorInfo, type FormEvent, type ReactNode, useEffect, useMemo, useState } from 'react';
import logo from '../assets/streamfind.png';

import WorkflowCanvas from './WorkflowCanvas';
import { NotificationToasts } from './NotificationCenter';
import { StreamFindApiClient, type ProjectSession, type ServiceState } from '../framework/backend/StreamFindApiClient';
import type {
  BackendCapability,
  CapabilityPort,
  CapabilityParameter,
  JsonSchema,
  ServiceCapabilities,
  TableColumnContract,
  TableContract,
  WorkflowState,
} from '../framework/backend/protocol';
import { notifyApp } from '../framework/notifications/notificationBus';
import {
  forceDuckDbPath,
  navigateHash,
  navItems,
  palettes,
  projectNameFromPath,
  routeFromHash,
  styles,
  uniqueSessionId,
  type NavItem,
  type Palette,
  type ParsedRoute,
  type RouteKey,
  type Style,
} from './shell/appShellModel';

type ThemeMode = 'light' | 'dark';
type WorkflowSummary = {
  operationCount: number;
  revision: number;
  state: WorkflowState;
  artifactCount: number;
};
type OntologyEntry = {
  id: string;
  label: string;
  kind: 'table' | 'operation' | 'api-command' | 'method' | 'parameter' | 'field' | 'result';
  definition: string;
  table?: TableContract;
  capability?: BackendCapability;
  column?: TableColumnContract;
  parentId?: string;

  parameter?: CapabilityParameter;
  usages?: string[];
  dataKind?: string;
};

function ontologyEntries(capabilities: ServiceCapabilities): OntologyEntry[] {
  const entries = new Map<string, OntologyEntry>();
  const normalizeColumn = (column: TableColumnContract): TableColumnContract => {
    const type = (column.type || '').toLowerCase();
    const storage = (column.duckdb_type || '').toLowerCase();
    const primitive = column.primitive_type && column.primitive_type !== 'unknown' ? column.primitive_type : undefined;
    const inferred =
      type === 'real' || type === 'number' || storage === 'double' || storage === 'float'
        ? 'double'
        : type === 'integer' || storage.includes('int')
          ? 'integer'
          : type === 'boolean' || storage === 'boolean'
            ? 'boolean'
            : type === 'timestamp' || storage.includes('timestamp') || storage === 'date'
              ? 'timestamp'
              : type === 'object' || storage === 'struct'
                ? 'struct'
                : type === 'array' || storage === 'list'
                  ? 'list'
                  : 'varchar';
    return { ...column, primitive_type: primitive || inferred };
  };
  const schemaColumn = (name: string, schema: JsonSchema, required: boolean): TableColumnContract => {
    const type = Array.isArray(schema.type) ? schema.type[0] : schema.type;
    const storageType = typeof schema.duckdb_type === 'string' ? schema.duckdb_type : undefined;
    const primitiveType =
      type === 'number' || type === 'real' || type === 'float' || type === 'double'
        ? 'double'
        : type === 'boolean'
          ? 'boolean'
          : type === 'integer'
            ? 'integer'
            : type === 'timestamp' || type === 'date'
              ? 'timestamp'
              : type === 'array'
                ? 'list'
                : type === 'object'
                  ? 'struct'
                  : type === 'string'
                    ? 'varchar'
                    : 'unknown';
    const column: TableColumnContract = {
      name,
      type,
      primitive_type: primitiveType,
      duckdb_type: storageType || primitiveType,
      nullable: !required,
      description: schema.description,
    };
    if (type === 'array' && schema.items) {
      column.element = schemaColumn('element', schema.items, true);
    }
    if (type === 'object' && schema.properties) {
      column.fields = Object.entries(schema.properties).map(([fieldName, fieldSchema]) =>
        schemaColumn(fieldName, fieldSchema as JsonSchema, (schema.required || []).includes(fieldName)),
      );
    }
    return column;
  };
  const tableFromPort = (port: CapabilityPort): TableContract | undefined => {
    if (port.table) return port.table;
    if (port.data_kind !== 'duckdb_table' || !port.schema?.properties) return undefined;
    const required = new Set(port.schema.required || []);
    return {
      table_contract_name: port.id,
      description: port.schema.description,
      columns: Object.entries(port.schema.properties).map(([name, schema]) =>
        schemaColumn(name, schema as JsonSchema, required.has(name)),
      ),
    };
  };
  const tableFromParameter = (parameter: CapabilityParameter): TableContract | undefined => {
    if (parameter.schema.type !== 'table' || !parameter.schema.properties) return undefined;
    const required = new Set(parameter.schema.required || []);
    return {
      table_contract_name: parameter.name,
      description: parameter.description,
      columns: Object.entries(parameter.schema.properties).map(([name, schema]) =>
        schemaColumn(name, schema as JsonSchema, required.has(name)),
      ),
    };
  };
  const addTable = (table: TableContract | undefined) => {
    if (!table?.table_contract_name) return;
    const existing = entries.get(table.table_contract_name);
    if (existing?.table) {
      const columns = new Map(existing.table.columns.map((column) => [column.name, column]));
      table.columns.forEach((column) => {
        const normalized = normalizeColumn(column);
        columns.set(normalized.name, { ...columns.get(normalized.name), ...normalized });
      });
      existing.table = { ...existing.table, ...table, columns: [...columns.values()] };
      if (!existing.definition || existing.definition === 'Ontology table contract.')
        existing.definition = table.description || existing.definition;
      return;
    }
    entries.set(table.table_contract_name, {
      id: table.table_contract_name,
      label: table.table_contract_name,
      kind: 'table',
      definition: table.description || 'Ontology table contract.',
      table: { ...table, columns: (table.columns || []).map(normalizeColumn) },
    });
  };
  const addTableContracts = (value: string[] | TableContract[] | undefined) => {
    if (Array.isArray(value)) value.filter((item): item is TableContract => typeof item !== 'string').forEach(addTable);
  };
  capabilities.tables?.forEach(addTable);
  capabilities.operations.forEach((capability) => {
    addTableContracts(capability.effects.reads);
    addTableContracts(capability.effects.writes);
    addTableContracts(capability.effects.conditional_reads);

    [
      ...(capability.canvas?.input_ports || []),
      ...(capability.input_ports || []),
      ...(capability.inputs || []),
    ].forEach((port) => addTable(tableFromPort(port)));
    [
      ...((capability.canvas as { output_ports?: CapabilityPort[] } | undefined)?.output_ports || []),
      ...(capability.output_ports || []),
      ...(capability.outputs || []),
    ].forEach((port) => addTable(tableFromPort(port)));
  });
  const tableEntries = [...entries.values()];

  const operationEntries = capabilities.operations.map((capability) => ({
    id: capability.canonical_id,
    label: capability.label,
    kind:
      capability.kind === 'command'
        ? ('api-command' as const)
        : capability.kind === 'method'
          ? ('method' as const)
          : ('operation' as const),
    definition: capability.definition,
    capability,
  }));
  const parameterMap = new Map<string, OntologyEntry>();
  capabilities.operations.forEach((capability) => {
    capability.parameters.forEach((parameter) => {
      const existing = parameterMap.get(parameter.name);
      if (existing) {
        existing.usages = [...(existing.usages || []), capability.label];
      } else {
        parameterMap.set(parameter.name, {
          id: parameter.name,
          label: parameter.label || parameter.name,
          kind: 'parameter',
          definition: parameter.description || 'Ontology parameter.',
          parameter,
          table: tableFromParameter(parameter),
          usages: [capability.label],
        });
      }
    });
  });
  const parameterEntries = [...parameterMap.values()];
  const portEntries = new Map<string, OntologyEntry>();
  capabilities.operations.forEach((capability) => {
    const ports = [
      ...(capability.canvas?.input_ports || []),
      ...(capability.input_ports || []),
      ...(capability.inputs || []),
      ...((capability.canvas as { output_ports?: CapabilityPort[] } | undefined)?.output_ports || []),
      ...(capability.output_ports || []),
      ...(capability.outputs || []),
    ];
    ports.forEach((port) => {
      const id = port.table?.table_contract_name || port.semantic_contract || port.id;
      const table = tableFromPort(port);
      const dataKind = 'data_kind' in port ? port.data_kind : undefined;
      if (!portEntries.has(id) && !entries.has(id)) {
        portEntries.set(id, {
          id,
          label: port.table?.table_contract_name || port.label || id,
          kind: 'result',
          definition: port.table?.description || port.schema?.description || 'Ontology port contract.',
          table,
          dataKind: dataKind || 'value',
        });
      }
    });
  });

  return Array.from(
    new Map(
      [...tableEntries, ...operationEntries, ...parameterEntries, ...portEntries.values()].map((entry) => [
        entry.id,
        entry,
      ]),
    ).values(),
  );
}

function columnType(column: TableColumnContract): string {
  if (column.duckdb_type && column.duckdb_type.toLowerCase() !== 'unknown') return column.duckdb_type;
  if (column.type && column.type.toLowerCase() !== 'unknown') return column.type;
  if (column.primitive_type && column.primitive_type.toLowerCase() !== 'unknown') return column.primitive_type;
  return 'value';
}

function schemaTypeLabel(schema: JsonSchema): string {
  const type = Array.isArray(schema.type) ? schema.type.join(' | ') : schema.type || 'value';
  if (type === 'array' && schema.items) return `array<${schemaTypeLabel(schema.items)}>`;
  return type;
}

function OntologyTermLink({
  term,
  onOpenTerm,
  children,
}: {
  term: string;
  onOpenTerm: (term: string) => void;
  children: ReactNode;
}) {
  return (
    <button type="button" className="sf-ontology-entry-link sf-ontology-detail-link" onClick={() => onOpenTerm(term)}>
      {children}
    </button>
  );
}

function OntologyColumn({
  column,
  depth = 0,
  parentId,
  onOpenTerm,
  onOpenColumn,
}: {
  column: TableColumnContract;
  depth?: number;
  parentId?: string;
  onOpenTerm: (term: string) => void;
  onOpenColumn: (column: TableColumnContract, parentId?: string) => void;
}) {
  return (
    <li style={{ marginLeft: depth * 14 }}>
      <strong>
        <button
          type="button"
          className="sf-ontology-entry-link sf-ontology-detail-link"
          onClick={() => onOpenColumn(column, parentId)}
        >
          {column.name}
        </button>
      </strong>
      <span>
        {columnType(column)} · {column.nullable === false ? 'required' : 'nullable'}
        {` · ${column.description || `Column “${column.name}” in this table contract.`}`}
      </span>
      {column.element ? (
        <ul>
          <OntologyColumn
            column={column.element}
            depth={depth + 1}
            parentId={parentId}
            onOpenTerm={onOpenTerm}
            onOpenColumn={onOpenColumn}
          />
        </ul>
      ) : null}
      {column.fields?.length ? (
        <ul>
          {column.fields.map((field) => (
            <OntologyColumn
              key={field.name}
              column={field}
              depth={depth + 1}
              parentId={parentId}
              onOpenTerm={onOpenTerm}
              onOpenColumn={onOpenColumn}
            />
          ))}
        </ul>
      ) : null}
    </li>
  );
}

function OntologyCapabilityDetails({
  capability,
  onOpenTerm,
}: {
  capability: BackendCapability;
  onOpenTerm: (term: string) => void;
}) {
  const inputs: CapabilityPort[] = [
    ...(capability.canvas?.input_ports || []),
    ...(capability.input_ports || []),
    ...(capability.inputs || []),
  ];
  const outputs: CapabilityPort[] = [
    ...((capability.canvas as { output_ports?: CapabilityPort[] } | undefined)?.output_ports || []),
    ...(capability.output_ports || []),
    ...(capability.outputs || []),
  ];
  const portLabel = (port: CapabilityPort) => port.table?.table_contract_name || port.label || port.id;
  const portTerm = (port: CapabilityPort) => port.table?.table_contract_name || port.semantic_contract || port.id;
  return (
    <>
      {capability.parameters.length ? (
        <section>
          <h3>Parameters</h3>
          {capability.parameters.map((parameter) => (
            <div className="sf-ontology-detail" key={parameter.name}>
              <strong>
                <OntologyTermLink term={parameter.name} onOpenTerm={onOpenTerm}>
                  {parameter.label || parameter.name}
                </OntologyTermLink>
              </strong>
              <span>{parameter.description || 'No definition.'}</span>
              <small>
                type: {schemaTypeLabel(parameter.schema)}
                {parameter.default !== undefined ? ` · default: ${JSON.stringify(parameter.default)}` : ''}
              </small>
            </div>
          ))}
        </section>
      ) : null}
      {inputs.length ? (
        <section>
          <h3>Inputs</h3>
          {inputs.map((port) => (
            <div className="sf-ontology-detail" key={port.id}>
              <strong>
                <OntologyTermLink term={portTerm(port)} onOpenTerm={onOpenTerm}>
                  {portLabel(port)}
                </OntologyTermLink>
              </strong>
              <span>{port.table?.description || port.schema?.description || port.id}</span>
              <small>
                type: {port.data_kind || 'value'}
                {port.table ? ` · ${port.table.table_contract_name}` : ''}
              </small>
            </div>
          ))}
        </section>
      ) : null}
      {outputs.length ? (
        <section>
          <h3>Outputs</h3>
          {outputs.map((port) => (
            <div className="sf-ontology-detail" key={port.id}>
              <strong>
                <OntologyTermLink term={portTerm(port)} onOpenTerm={onOpenTerm}>
                  {portLabel(port)}
                </OntologyTermLink>
              </strong>
              <span>{port.table?.description || port.schema?.description || port.id}</span>
              <small>
                type: {port.data_kind || 'value'}
                {port.table ? ` · ${port.table.table_contract_name}` : ''}
              </small>
            </div>
          ))}
        </section>
      ) : null}
    </>
  );
}

function OntologyWiki({
  capabilities,
  initialTerm,
  onClose,
}: {
  capabilities: ServiceCapabilities;
  initialTerm?: string;
  onClose: () => void;
}) {
  const entries = useMemo(() => ontologyEntries(capabilities), [capabilities]);
  const [query, setQuery] = useState(initialTerm || '');
  const [selectedId, setSelectedId] = useState(initialTerm || entries[0]?.id || '');
  const [selectedColumn, setSelectedColumn] = useState<{ column: TableColumnContract; parentId?: string } | null>(null);
  const [history, setHistory] = useState<string[]>([]);
  const selectTerm = (term: string) => {
    setSelectedColumn(null);
    if (term === selectedId) return;
    if (selectedId) setHistory((items) => [...items, selectedId]);
    setSelectedId(term);
  };
  const selectColumn = (column: TableColumnContract, parentId?: string) => setSelectedColumn({ column, parentId });
  const goBack = () => {
    const previous = history.at(-1);
    if (!previous) return;
    setHistory((items) => items.slice(0, -1));
    setSelectedId(previous);
  };
  const filtered = entries.filter((entry) =>
    `${entry.id} ${entry.label} ${entry.definition}`.toLowerCase().includes(query.trim().toLowerCase()),
  );
  const selected = entries.find((entry) => entry.id === selectedId) || filtered[0] || entries[0];
  return (
    <section className="sf-ontology-wiki" aria-label="Ontology wiki">
      <header className="sf-ontology-wiki-header">
        <div>
          <h1>Search the streamfind glossary</h1>
        </div>
        <div className="sf-ontology-wiki-header-actions">
          <button
            type="button"
            className="sf-icon-button"
            aria-label="Go back in ontology wiki"
            onClick={goBack}
            disabled={!history.length}
          >
            <i className="fa-solid fa-arrow-left" />
          </button>
          <button type="button" className="sf-icon-button" aria-label="Close ontology wiki" onClick={onClose}>
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
      </header>
      <div className="sf-ontology-wiki-body">
        <aside className="sf-ontology-wiki-index">
          <input
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="Search ontology terms"
            aria-label="Search ontology terms"
          />
          <div className="sf-ontology-wiki-count">{filtered.length} terms</div>
          {filtered.map((entry) => (
            <button
              key={entry.id}
              type="button"
              className={`sf-ontology-entry-link ${entry.id === selected?.id ? 'active' : ''}`}
              onClick={() => selectTerm(entry.id)}
            >
              <strong>{entry.label}</strong>
              <small>
                {entry.kind} · {entry.id}
              </small>
            </button>
          ))}
        </aside>
        <article className="sf-ontology-wiki-entry">
          {selectedColumn ? (
            <>
              <span className="sf-eyebrow">table column</span>
              <h2>{selectedColumn.column.name}</h2>
              <code>
                {selectedColumn.parentId
                  ? `${selectedColumn.parentId}.${selectedColumn.column.name}`
                  : selectedColumn.column.name}
              </code>
              <p>
                {selectedColumn.column.description || `Column “${selectedColumn.column.name}” in this table contract.`}
              </p>
              <div className="sf-ontology-type">
                type: {columnType(selectedColumn.column)} ·{' '}
                {selectedColumn.column.nullable === false ? 'required' : 'nullable'}
              </div>
            </>
          ) : selected ? (
            <>
              <span className="sf-eyebrow">{selected.kind}</span>
              <h2>{selected.label}</h2>
              <code>{selected.id}</code>
              <p>{selected.definition}</p>
              {selected.kind === 'result' && !selected.table ? (
                <div className="sf-ontology-type">type: {selected.dataKind || 'value'}</div>
              ) : null}
              {selected.table ? (
                <section>
                  <h3>Table contract</h3>
                  <div className="sf-ontology-type">type: {selected.dataKind || 'table'}</div>
                  <ul className="sf-ontology-columns">
                    {selected.table.columns.map((column) => (
                      <OntologyColumn
                        key={column.name}
                        column={column}
                        parentId={selected.id}
                        onOpenTerm={selectTerm}
                        onOpenColumn={selectColumn}
                      />
                    ))}
                  </ul>
                </section>
              ) : null}
              {selected.parameter ? (
                <section>
                  <h3>Parameter contract</h3>
                  <div className="sf-ontology-type">type: {schemaTypeLabel(selected.parameter.schema)}</div>
                  {selected.parameter.default !== undefined ? (
                    <div className="sf-ontology-type">default: {JSON.stringify(selected.parameter.default)}</div>
                  ) : null}
                  {selected.usages?.length ? (
                    <div className="sf-ontology-type">
                      <strong>Used by</strong>
                      <ul className="sf-ontology-usage-list">
                        {selected.usages.map((usage) => (
                          <li key={usage}>{usage}</li>
                        ))}
                      </ul>
                    </div>
                  ) : null}
                </section>
              ) : null}
              {selected.capability ? (
                <OntologyCapabilityDetails capability={selected.capability} onOpenTerm={selectTerm} />
              ) : null}
            </>
          ) : (
            <p>No ontology terms match the search.</p>
          )}
        </article>
      </div>
    </section>
  );
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
        <span>{project.domains?.join(', ') || 'No domains'}</span>
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
        <span className="sf-project-preview-domain">{project.domains?.join(', ') || 'No domains'}</span>
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
  onOpenOntologyWiki,
}: {
  project: ProjectSession;
  capabilities: ServiceCapabilities;
  client: StreamFindApiClient;
  onProjectHub: () => void;
  onOpenOntologyWiki?: (term?: string) => void;
}) {
  return (
    <div className="sf-page sf-project-workspace">
      <WorkflowCanvas
        project={project}
        capabilities={capabilities}
        client={client}
        onProjectHub={onProjectHub}
        onOpenOntologyWiki={onOpenOntologyWiki}
      />
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
  const [ontologyWikiTerm, setOntologyWikiTerm] = useState<string | null>(null);

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      setSettingsOpen(false);
      setBackendOpen(false);
      setOntologyWikiTerm(null);
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
              className="sf-icon-button sf-ontology-wiki-button"
              onClick={() => setOntologyWikiTerm('')}
              aria-label="Open ontology wiki"
              title="Open ontology wiki"
            >
              <i className="fa-solid fa-book-open" />
            </button>
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
                onOpenOntologyWiki={(term) => setOntologyWikiTerm(term || '')}
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
        {ontologyWikiTerm !== null ? (
          <div className="sf-ontology-wiki-layer">
            <OntologyWiki
              capabilities={capabilities}
              initialTerm={ontologyWikiTerm}
              onClose={() => setOntologyWikiTerm(null)}
            />
          </div>
        ) : null}
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
