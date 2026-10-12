import type { ProjectSession } from '../../framework/backend/StreamFindApiClient';

export type Palette = 'streamfind' | 'playful' | 'matrix';
export type Style = 'classic' | 'studio' | 'chrome';
export type RouteKey = 'projects' | 'workflow' | 'results' | 'data' | 'runs' | 'provenance';
export type ParsedRoute = { route: RouteKey; sessionId?: string };
export type NavItem = { key: RouteKey; label: string; icon: string; hint: string };

export function projectNameFromPath(databasePath: string): string {
  return (
    databasePath
      .split(/[\\/]/)
      .pop()
      ?.replace(/\.[^.]+$/, '') || 'project'
  );
}

export function forceDuckDbPath(databasePath: string): string {
  const normalized = databasePath.trim();
  if (!normalized) return normalized;
  return /\.duckdb$/i.test(normalized) ? normalized : normalized.replace(/\.[^.\\/]+$/, '') + '.duckdb';
}

export function uniqueSessionId(projectName: string, projects: ProjectSession[]): string {
  const existing = new Set(projects.map((project) => project.session_id));
  if (!existing.has(projectName)) return projectName;
  let prefix = 1;
  while (existing.has(`${prefix}_${projectName}`)) prefix += 1;
  return `${prefix}_${projectName}`;
}

export const navItems: NavItem[] = [
  { key: 'projects', label: 'Project Hub', icon: 'fa-solid fa-house', hint: 'Create and open projects' },
  {
    key: 'workflow',
    label: 'Workflow',
    icon: 'fa-solid fa-diagram-project',
    hint: 'Compose ontology-driven workflows',
  },
  { key: 'results', label: 'Results', icon: 'fa-solid fa-wave-square', hint: 'Explore scientific outputs' },
  { key: 'data', label: 'Data', icon: 'fa-solid fa-boxes-stacked', hint: 'Inspect project tables' },
  { key: 'runs', label: 'Runs', icon: 'fa-solid fa-clock', hint: 'Monitor durable executions' },
  { key: 'provenance', label: 'Provenance', icon: 'fa-solid fa-book-open', hint: 'Trace results to their source' },
];

export const palettes: Array<{ id: Palette; label: string; description: string; swatches: string[] }> = [
  {
    id: 'streamfind',
    label: 'streamfind',
    description: 'White/black foundation with navy, green, and aqua accents.',
    swatches: ['#08296c', '#4c8333', '#78a7ff', '#83b95a'],
  },
  {
    id: 'playful',
    label: 'Playful',
    description: 'White/black foundation with orange, blue, and green accents.',
    swatches: ['#f07848', '#2864dc', '#78a7ff', '#67d391'],
  },
  {
    id: 'matrix',
    label: 'Matrix',
    description: 'White/black foundation with neon-green and lime accents.',
    swatches: ['#168a46', '#50e878', '#6c9f28', '#b6f36b'],
  },
];

export const styles: Array<{ id: Style; label: string; description: string }> = [
  { id: 'classic', label: 'Classic', description: 'Flat panels, sharp edges, and compact administration density.' },
  {
    id: 'studio',
    label: 'Studio',
    description: 'Editor-like proportions with restrained glow and focused workspaces.',
  },
  { id: 'chrome', label: 'Chrome', description: 'Rounded browser-inspired framing with polished navigation.' },
];

export function routeFromHash(): ParsedRoute {
  const parts = window.location.hash.replace(/^#\/?/, '').split('/').filter(Boolean);
  if (parts[0] === 'project' && parts[1]) {
    const route = parts[2] as RouteKey;
    return {
      route: navItems.some((item) => item.key === route && item.key !== 'projects') ? route : 'workflow',
      sessionId: decodeURIComponent(parts[1]),
    };
  }
  const route = parts[0] as RouteKey;
  return { route: navItems.some((item) => item.key === route) ? route : 'projects' };
}

export function navigateHash(key: RouteKey, sessionId?: string): void {
  const path = sessionId ? `#/project/${encodeURIComponent(sessionId)}/${key}` : `#/${key}`;
  window.history.pushState({}, '', path);
  window.dispatchEvent(new Event('hashchange'));
}
