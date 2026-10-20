import { useEffect, type ReactNode } from 'react';

type ViewerShellProps = {
  title: string;
  subtitle?: string;
  variant?: 'json' | 'wide' | 'visualization';
  onClose: () => void;
  /** Render a viewer without the standard artifact title row. */
  showHeader?: boolean;
  children: ReactNode;
};

export function ViewerShell({ title, subtitle, variant = 'json', onClose, showHeader = true, children }: ViewerShellProps) {
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [onClose]);

  return (
    <div className="sf-artifact-viewer-backdrop" role="presentation" onMouseDown={onClose}>
      <section
        className={`sf-artifact-viewer ${variant}${showHeader ? '' : ' sf-artifact-viewer-content-only'}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        onMouseDown={(event) => event.stopPropagation()}
      >
        {showHeader ? (
          <header>
            <div>
              <h2>{title}</h2>
              {subtitle ? <small>{subtitle}</small> : null}
            </div>
            <button type="button" className="sf-icon-button" aria-label="Close artifact viewer" onClick={onClose}>
              <i className="fa-solid fa-xmark" />
            </button>
          </header>
        ) : null}
        {!showHeader ? (
          <button type="button" className="sf-artifact-viewer-close sf-icon-button" aria-label="Close artifact viewer" onClick={onClose}>
            <i className="fa-solid fa-xmark" />
          </button>
        ) : null}
        {children}
      </section>
    </div>
  );
}
