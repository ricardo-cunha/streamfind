import { useEffect, useMemo, useRef, useState, type MouseEvent, type ReactNode } from 'react';
import { type FileSystemEntry, StreamFindApiClient } from '../backend/StreamFindApiClient';

type PathFileManagerProps = {
  client: StreamFindApiClient;
  selectedPaths: string[];
  onAddPaths: (paths: string[]) => void;
};

type TreeNode = FileSystemEntry & { children?: TreeNode[]; expanded?: boolean; loading?: boolean };

function pathSegments(path: string): Array<{ label: string; path: string }> {
  const normalized = path.replaceAll('/', '\\').replace(/[\\]+$/, '');
  if (!normalized) return [];
  const drive = /^[A-Za-z]:/.exec(normalized)?.[0];
  if (!drive) return [{ label: normalized, path: normalized }];
  const result = [{ label: drive, path: `${drive}\\` }];
  let current = `${drive}\\`;
  normalized
    .slice(drive.length)
    .split('\\')
    .filter(Boolean)
    .forEach((segment) => {
      current = `${current.replace(/[\\]+$/, '')}\\${segment}`;
      result.push({ label: segment, path: current });
    });
  return result;
}

function treeNodes(entries: FileSystemEntry[]): TreeNode[] {
  return entries.filter((entry) => entry.isDirectory).map((entry) => ({ ...entry }));
}

function updateTree(nodes: TreeNode[], path: string, update: (node: TreeNode) => TreeNode): TreeNode[] {
  return nodes.map((node) => {
    if (node.path === path) return update(node);
    return node.children ? { ...node, children: updateTree(node.children, path, update) } : node;
  });
}

function fileIcon(entry: FileSystemEntry): string {
  return entry.isDirectory ? 'fa-solid fa-folder sf-file-icon-folder' : 'fa-solid fa-file sf-file-icon-file';
}

export function PathFileManager({ client, selectedPaths, onAddPaths }: PathFileManagerProps) {
  const [currentPath, setCurrentPath] = useState('');
  const [entries, setEntries] = useState<FileSystemEntry[]>([]);
  const [tree, setTree] = useState<TreeNode[]>([]);
  const [sidebarWidth, setSidebarWidth] = useState(280);
  const [resizingSidebar, setResizingSidebar] = useState(false);
  const [search, setSearch] = useState('');
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [pendingPaths, setPendingPaths] = useState<string[]>(selectedPaths);
  const selectionAnchor = useRef<string | null>(null);
  const requestId = useRef(0);

  const loadPath = (path: string) => {
    const id = ++requestId.current;
    setLoading(true);
    setError(null);
    void client
      .browseFileSystem(path)
      .then((result) => {
        if (id !== requestId.current) return;
        setCurrentPath(result.path);
        setEntries(result.entries);
        setSearch('');
      })
      .catch((reason) => {
        if (id === requestId.current) setError(reason instanceof Error ? reason.message : 'Filesystem browse failed.');
      })
      .finally(() => {
        if (id === requestId.current) setLoading(false);
      });
  };

  const loadTreeFolder = (node: TreeNode) => {
    if (node.children) {
      setTree((current) => updateTree(current, node.path, (item) => ({ ...item, expanded: !item.expanded })));
      return;
    }
    setTree((current) => updateTree(current, node.path, (item) => ({ ...item, expanded: true, loading: true })));
    void client
      .browseFileSystem(node.path)
      .then((result) => {
        setTree((current) =>
          updateTree(current, node.path, (item) => ({
            ...item,
            children: treeNodes(result.entries),
            expanded: true,
            loading: false,
          })),
        );
      })
      .catch(() => setTree((current) => updateTree(current, node.path, (item) => ({ ...item, loading: false }))));
  };

  useEffect(() => {
    const timer = window.setTimeout(() => {
      const id = ++requestId.current;
      void client
        .browseFileSystem('')
        .then((result) => {
          if (id !== requestId.current) return;
          setTree(treeNodes(result.entries));
          setCurrentPath(result.path);
          setEntries(result.entries);
        })
        .catch((reason) => setError(reason instanceof Error ? reason.message : 'Filesystem browse failed.'))
        .finally(() => setLoading(false));
    }, 0);
    return () => window.clearTimeout(timer);
  }, [client]);

  useEffect(() => {
    if (!resizingSidebar) return undefined;
    const move = (event: PointerEvent) =>
      setSidebarWidth((width) => Math.min(520, Math.max(180, width + event.movementX)));
    const stop = () => setResizingSidebar(false);
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', stop, { once: true });
    return () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', stop);
    };
  }, [resizingSidebar]);

  const visibleEntries = useMemo(() => {
    const query = search.trim().toLocaleLowerCase();
    return entries
      .filter((entry) => !query || entry.name.toLocaleLowerCase().includes(query))
      .sort((left, right) => {
        if (left.isDirectory !== right.isDirectory) return left.isDirectory ? -1 : 1;
        return left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: 'base' });
      });
  }, [entries, search]);

  const selectEntry = (entry: FileSystemEntry, event: MouseEvent, contextSelection = false) => {
    event.preventDefault();
    const entryIndex = visibleEntries.findIndex((candidate) => candidate.path === entry.path);
    const anchorIndex = selectionAnchor.current
      ? visibleEntries.findIndex((candidate) => candidate.path === selectionAnchor.current)
      : -1;
    if (event.shiftKey && anchorIndex >= 0 && entryIndex >= 0) {
      const start = Math.min(anchorIndex, entryIndex);
      const end = Math.max(anchorIndex, entryIndex);
      const range = visibleEntries.slice(start, end + 1).map((candidate) => candidate.path);
      setPendingPaths((current) => [...current, ...range.filter((path) => !current.includes(path))]);
    } else if (event.ctrlKey || event.metaKey || contextSelection) {
      setPendingPaths((current) =>
        current.includes(entry.path) ? current.filter((path) => path !== entry.path) : [...current, entry.path],
      );
    } else {
      setPendingPaths([entry.path]);
    }
    selectionAnchor.current = entry.path;
  };

  const selectTreeFolder = (node: TreeNode) => {
    loadPath(node.path);
  };

  const renderTree = (nodes: TreeNode[], depth = 0): ReactNode =>
    nodes.map((node) => (
      <div className="sf-file-tree-node" key={node.path}>
        <div
          className={`sf-file-tree-row${currentPath === node.path ? ' selected' : ''}`}
          style={{ paddingLeft: `${8 + depth * 16}px` }}
        >
          <button
            type="button"
            className="sf-file-tree-toggle"
            aria-label={`${node.expanded ? 'Collapse' : 'Expand'} ${node.name}`}
            onClick={() => loadTreeFolder(node)}
          >
            <i className={`fa-solid fa-chevron-${node.expanded ? 'down' : 'right'}`} />
          </button>
          <button type="button" className="sf-file-tree-label" onClick={() => selectTreeFolder(node)}>
            <i className="fa-solid fa-folder sf-file-icon-folder" /> {node.name.replace(/[\\/]$/, '')}
          </button>
          {node.loading ? <i className="fa-solid fa-spinner fa-spin sf-file-tree-loading" /> : null}
        </div>
        {node.expanded && node.children ? renderTree(node.children, depth + 1) : null}
      </div>
    ));

  return (
    <div className="sf-windows-file-picker">
      <div className="sf-windows-toolbar">
        <button type="button" aria-label="Refresh" disabled={loading} onClick={() => loadPath(currentPath)}>
          <i className="fa-solid fa-rotate-right" />
        </button>
        <div className="sf-windows-address-bar" aria-label="Current folder">
          {pathSegments(currentPath).map((segment) => (
            <span className="sf-windows-breadcrumb" key={segment.path}>
              <i className="fa-solid fa-chevron-right" />
              <button type="button" onClick={() => loadPath(segment.path)}>
                {segment.label}
              </button>
            </span>
          ))}
        </div>
        <label className="sf-windows-search">
          <i className="fa-solid fa-magnifying-glass" />
          <input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search" />
        </label>
      </div>
      <div className="sf-windows-content" style={{ gridTemplateColumns: `${sidebarWidth}px 6px minmax(0, 1fr)` }}>
        <aside className="sf-windows-sidebar" aria-label="Folder tree">
          <div className="sf-windows-sidebar-section">
            {loading && !tree.length ? <span>Loading drives…</span> : renderTree(tree)}
          </div>
        </aside>
        <button
          type="button"
          className="sf-file-tree-resizer"
          aria-label="Resize folder tree"
          onPointerDown={() => setResizingSidebar(true)}
        />
        <main className="sf-windows-file-list" aria-label="Files and folders">
          <div className="sf-windows-file-list-header">
            <span>Name</span>
            <span>Date modified</span>
            <span>Type</span>
            <span>Size</span>
          </div>
          {error ? <div className="sf-windows-empty sf-json-editor-error">{error}</div> : null}
          {loading ? <div className="sf-windows-empty">Loading…</div> : null}
          {!loading && !error && !visibleEntries.length ? (
            <div className="sf-windows-empty">This folder is empty.</div>
          ) : null}
          {visibleEntries.map((entry) => (
            <button
              type="button"
              className={`sf-windows-file-row${pendingPaths.includes(entry.path) ? ' selected' : ''}`}
              key={entry.path}
              onClick={(event) => selectEntry(entry, event)}
              onContextMenu={(event) => selectEntry(entry, event, true)}
              onDoubleClick={() => entry.isDirectory && loadPath(entry.path)}
            >
              <span>
                <i className={fileIcon(entry)} /> {entry.name.replace(/[\\/]$/, '')}
              </span>
              <span>—</span>
              <span>{entry.isDirectory ? 'File folder' : 'File'}</span>
              <span>{entry.size ? `${Math.ceil(entry.size / 1024)} KB` : '—'}</span>
            </button>
          ))}
        </main>
      </div>
      <div className="sf-windows-file-name-bar">
        <span className="sf-windows-selection-count" aria-live="polite">
          {pendingPaths.length} path{pendingPaths.length === 1 ? '' : 's'} selected
        </span>
        <span className="sf-windows-selection-help">Shift-click or right-click to add a batch.</span>
        <button
          type="button"
          className="sf-button"
          disabled={!pendingPaths.length}
          onClick={() => {
            onAddPaths(pendingPaths);
            setPendingPaths([]);
            selectionAnchor.current = null;
          }}
        >
          <i className="fa-solid fa-plus" /> Add selected paths
        </button>
      </div>
    </div>
  );
}
