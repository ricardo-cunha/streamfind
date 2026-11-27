import { useState } from 'react';
import { PathFileManager } from './PathFileManager';
import type { StreamFindApiClient } from '../framework/backend/StreamFindApiClient';

type ProjectFolderPickerProps = {
  client: StreamFindApiClient;
  mode: 'parent' | 'project';
  initialPath: string;
  selectedPath?: string;
  onSelect: (path: string) => void;
  onClose: () => void;
};

export function ProjectFolderPicker({ client, mode, initialPath, selectedPath = '', onSelect, onClose }: ProjectFolderPickerProps) {
  const heading = mode === 'project' ? 'Choose a StreamFind project' : 'Choose a parent folder';
  const description = mode === 'project'
    ? 'Browse to a folder and select a .sf project in the right pane.'
    : 'Select the location where the new .sf project folder will be created.';
  const [selection, setSelection] = useState(selectedPath);

  return (
    <div className="sf-dialog-backdrop sf-project-folder-wizard-backdrop" onMouseDown={(event) => {
      if (event.target === event.currentTarget) onClose();
    }}>
      <section
        className="sf-json-editor-modal sf-path-wizard-modal sf-project-folder-wizard"
        role="dialog"
        aria-modal="true"
        aria-label={heading}
        onMouseDown={(event) => event.stopPropagation()}
        onWheel={(event) => event.stopPropagation()}
      >
        <header>
          <div>
            <h2>{heading}</h2>
            <small>{description}</small>
          </div>
          <button type="button" className="sf-icon-button" aria-label="Close folder picker" onClick={onClose}>
            <i className="fa-solid fa-xmark" />
          </button>
        </header>
        <PathFileManager
          client={client}
          mode={mode === 'project' ? 'project-folder' : 'folder'}
          initialPath={initialPath}
          selectedPaths={selectedPath ? [selectedPath] : []}
          onAddPaths={() => undefined}
          onProjectSelectionChange={setSelection}
        />
        <footer>
          <button type="button" className="sf-button secondary" onClick={onClose}>Cancel</button>
          <button
            type="button"
            className="sf-button secondary"
            disabled={!selection}
            onClick={() => selection && onSelect(selection)}
          >
            Choose selected folder
          </button>
        </footer>
      </section>
    </div>
  );
}
