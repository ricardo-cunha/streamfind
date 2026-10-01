import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { ViewerResolver } from './ViewerResolver';
import { ViewerRegistry, type ViewerContext } from './viewerTypes';

const context: ViewerContext = {
  sessionId: 'session-a',
  artifactId: 'artifact-a',
  semanticType: 'sfvis:visualizationSpecResult',
};

afterEach(() => cleanup());

describe('viewer registry', () => {
  it('resolves a viewer by the artifact semantic contract', () => {
    const registry = new ViewerRegistry();
    registry.register({
      id: 'test.viewer',
      label: 'Test viewer',
      accepts: ['visualizationSpecResult'],
      component: ({ context: viewerContext }) => <div>Viewing {viewerContext.artifactId}</div>,
    });

    render(<ViewerResolver registry={registry} context={context} />);

    expect(screen.getByText('Viewing artifact-a')).toBeInTheDocument();
  });

  it('renders the fallback when no viewer accepts the artifact', () => {
    const registry = new ViewerRegistry();

    render(
      <ViewerResolver
        registry={registry}
        context={{ ...context, semanticType: 'sf:table' }}
        fallback={<div>Generic artifact fallback</div>}
      />,
    );

    expect(screen.getByText('Generic artifact fallback')).toBeInTheDocument();
  });
});
