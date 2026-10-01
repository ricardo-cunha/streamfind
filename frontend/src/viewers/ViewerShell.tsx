import { useEffect, type ReactNode } from 'react';

type ViewerShellProps = {
  title: string;
  subtitle?: string;
  variant?: 'json' | 'wide' | 'visualization';
  onClose: () => void;
  children: ReactNode;
};

export function ViewerShell({ title, subtitle, variant = 'json', onClose, children }: ViewerShellProps) {
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
        className={`sf-artifact-viewer ${variant}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header>
          <div>
            <h2>{title}</h2>
            {subtitle ? <small>{subtitle}</small> : null}
          </div>
          <button type="button" className="sf-icon-button" aria-label="Close artifact viewer" onClick={onClose}>
            <i className="fa-solid fa-xmark" />
          </button>
        </header>
        {children}
      </section>
    </div>
  );
}
