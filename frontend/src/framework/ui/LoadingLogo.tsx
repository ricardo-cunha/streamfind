import type { ReactNode } from 'react';
import logo from '../../assets/streamfind.png';
import './LoadingLogo.css';

export function LoadingLogo({
  label,
  compact = false,
  size = 'default',
}: {
  label?: ReactNode;
  compact?: boolean;
  size?: 'default' | 'large';
}): ReactNode {
  return (
    <div
      className={`sf-loading-logo${compact ? ' sf-loading-logo-compact' : ''}${size === 'large' ? ' sf-loading-logo-large' : ''}`}
      role="status"
      aria-live="polite"
    >
      <img src={logo} alt="" aria-hidden="true" />
      {label ? <span>{label}</span> : null}
    </div>
  );
}
