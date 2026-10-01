import type { ReactNode } from 'react';
import { viewerRegistry, type ViewerContext, type ViewerRegistry } from './viewerTypes';

type ViewerResolverProps = {
  registry?: ViewerRegistry;
  context: ViewerContext;
  fallback?: ReactNode;
};

export function ViewerResolver({ registry = viewerRegistry, context, fallback }: ViewerResolverProps): ReactNode {
  const viewer = registry.resolve(context.semanticType);
  if (!viewer) return fallback ?? null;
  const Component = viewer.component;
  return <Component context={context} />;
}
