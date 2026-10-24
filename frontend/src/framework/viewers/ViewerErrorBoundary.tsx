import { Component, type ErrorInfo, type ReactNode } from 'react';

type ViewerErrorBoundaryProps = { children: ReactNode; resultId: string };
type ViewerErrorBoundaryState = { error: Error | null };

export class ViewerErrorBoundary extends Component<ViewerErrorBoundaryProps, ViewerErrorBoundaryState> {
  state: ViewerErrorBoundaryState = { error: null };

  static getDerivedStateFromError(error: Error): ViewerErrorBoundaryState {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.error(`Result viewer failed: ${this.props.resultId}`, error, info.componentStack);
  }

  render(): ReactNode {
    if (!this.state.error) return <div className="sf-viewer-error-boundary">{this.props.children}</div>;
    return (
      <div className="sf-viewer-error-boundary sf-artifact-viewer-error" role="alert">
        <strong>Viewer failed</strong>
        <span>{this.state.error.message}</span>
      </div>
    );
  }
}
