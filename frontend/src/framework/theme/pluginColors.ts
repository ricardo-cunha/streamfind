const bundledPluginColors: Record<string, string> = {
  core: 'var(--sf-plugin-core)',
  streamfind: 'var(--sf-plugin-streamfind)',
  mass_spec: 'var(--sf-plugin-mass-spec)',
  'mass-spec': 'var(--sf-plugin-mass-spec)',
  raman: 'var(--sf-plugin-raman)',
  sensors: 'var(--sf-plugin-sensors)',
  statistics: 'var(--sf-plugin-statistics)',
};

export function normalizePluginId(pluginId?: string): string {
  const normalized = (pluginId || 'core').trim().toLowerCase().replace(/^native\.plugin\./, '');
  return normalized || 'core';
}

export function pluginClassName(pluginId?: string): string {
  return normalizePluginId(pluginId).replace(/[^a-z0-9]+/g, '-');
}

function customPluginColor(pluginId: string): string {
  let hash = 2166136261;
  for (let index = 0; index < pluginId.length; index += 1) {
    hash ^= pluginId.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  const hue = (hash >>> 0) % 360;
  return `hsl(${hue} 68% 56%)`;
}

export function pluginColor(pluginId?: string): string {
  const normalized = normalizePluginId(pluginId);
  return bundledPluginColors[normalized] ?? customPluginColor(normalized);
}

export function blendPluginColors(pluginIds?: string[]): string {
  const colors = [...new Set((pluginIds?.length ? pluginIds : ['core']).map(pluginColor))];
  return colors.slice(1).reduce(
    (blend, color, index) =>
      `color-mix(in srgb, ${blend} ${((index + 1) / (index + 2)) * 100}%, ${color} ${100 / (index + 2)}%)`,
    colors[0],
  );
}
