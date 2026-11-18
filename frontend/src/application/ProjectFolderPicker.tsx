import { useEffect, useState } from 'react';
import type { FileSystemEntry, StreamFindApiClient } from '../framework/backend/StreamFindApiClient';

type ProjectFolderPickerProps = {
  client: StreamFindApiClient;
  mode: 'parent' | 'project';
  initialPath: string;
  onSelect: (path: string) => void;
  onClose: () => void;
};

export function ProjectFolderPicker({ client, mode, initialPath, onSelect, onClose }: ProjectFolderPickerProps) {
  const [path, setPath] = useState(initialPath);
  const [entries, setEntries] = useState<FileSystemEntry[]>([]);
  const [selected, setSelected] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);

  const load = (target: string) => {
    setLoading(true);
    setError('');
    void client
      .browseFileSystem(target)
      .then((result) => {
        setPath(result.path);
        setEntries(result.entries.filter((entry) => entry.isDirectory &&
          (mode === 'parent' || entry.name.toLocaleLowerCase().endsWith('.sf'))));
        setSelected('');
      })
      .catch((reason) => setError(reason instanceof Error ? reason.message : 'Folder listing failed.'))
      .finally(() => setLoading(false));
  };

  useEffect(() => load(initialPath), [client, initialPath, mode]);

  const choose = () => {
    const value = mode === 'parent' ? path : selected;
    if (value) onSelect(value);
  };

  return (
    <div className="sf-dialog-backdrop sf-project-folder-picker-backdrop" onMouseDown={(event) => {
      if (event.target === event.currentTarget) onClose();
    }}>
      <section className="sf-dialog sf-project-folder-picker" role="dialog" aria-modal="true" aria-label="Choose project folder">
        <div className="sf-dialog-heading">
          <div><h2>{mode === 'parent' ? 'Choose parent folder' : 'Choose .sf project folder'}</h2></div>
          <button type="button" className="sf-icon-button" aria-label="Close folder picker" onClick={onClose}>
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
        <form className="sf-project-folder-path" onSubmit={(event) => { event.preventDefault(); load(path); }}>
          <input aria-label="Folder path" value={path} onChange={(event) => setPath(event.target.value)} />
          <button className="sf-button secondary" type="submit">Go</button>
        </form>
        <div className="sf-project-folder-list" aria-label="Folders">
          {loading ? <p>Loading folders…</p> : null}
          {error ? <p className="sf-json-editor-error">{error}</p> : null}
          {!loading && !error && !entries.length ? <p>No folders in this location.</p> : null}
          {entries.map((entry) => (
            <button
              type="button"
              key={entry.path}
              className={selected === entry.path ? 'selected' : ''}
              onClick={() => setSelected(entry.path)}
              onDoubleClick={() => {
                if (mode === 'project' && entry.name.toLocaleLowerCase().endsWith('.sf')) setSelected(entry.path);
                else load(entry.path);
              }}
            >
              <i className="fa-solid fa-folder" /> {entry.name}
            </button>
          ))}
        </div>
        <div className="sf-dialog-actions">
          <button type="button" className="sf-button secondary" onClick={onClose}>Cancel</button>
          <button type="button" className="sf-button" onClick={choose} disabled={mode === 'project' && !selected}>
            {mode === 'parent' ? 'Use this parent folder' : 'Open selected project'}
          </button>
        </div>
      </section>
    </div>
  );
}
