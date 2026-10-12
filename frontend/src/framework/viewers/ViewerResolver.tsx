import type { ReactNode } from 'react';
import { viewerRegistry, type ViewerContext, type ViewerRegistry } from './viewerTypes';

type ViewerResolverProps = {
  registry?: ViewerRegistry;
  context: ViewerContext;
  viewerId?: string;
  fallback?: ReactNode;
};

export function ViewerResolver({
  registry = viewerRegistry,
  context,
  viewerId,
  fallback,
}: ViewerResolverProps): ReactNode {
  const viewer = viewerId
    ? registry.get(viewerId)
    : registry.resolve({ semanticType: context.semanticType, representation: context.artifact?.representation });
  if (!viewer) return fallback ?? null;
  const Component = viewer.component;
  return <Component context={context} />;
}
