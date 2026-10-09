import type { ReactNode } from 'react';
import logo from '../../assets/streamfind.png';
import './LoadingLogo.css';

export function LoadingLogo({ label, compact = false }: { label?: ReactNode; compact?: boolean }): ReactNode {
  return (
    <div className={`sf-loading-logo${compact ? ' sf-loading-logo-compact' : ''}`} role="status" aria-live="polite">
      <img src={logo} alt="" aria-hidden="true" />
      {label ? <span>{label}</span> : null}
    </div>
  );
}
