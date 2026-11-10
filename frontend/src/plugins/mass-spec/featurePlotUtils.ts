export function stablePlotGroup(row: Record<string, string | null | undefined>, groupBy: string): string {
  const selected = String(row[groupBy] ?? '').trim();
  if (groupBy !== 'replicate') return selected || String(row.analysis ?? '').trim() || '(unassigned)';
  const normalized = selected.toLowerCase();
  if (normalized === 'plant' || normalized === 'substrate') return normalized;
  const analysis = String(row.analysis ?? '').trim();
  const sampleClass = analysis.match(/^(plant|substrate)(?:[_\s]|$)/i)?.[1];
  return sampleClass?.toLowerCase() ?? (selected || '(unassigned)');
}

export function colorFor(value: string): string {
  const palette = [
    '#0072b2',
    '#d55e00',
    '#009e73',
    '#cc79a7',
    '#e69f00',
    '#56b4e9',
    '#f0e442',
    '#882255',
    '#117733',
    '#332288',
    '#aa4499',
    '#44aa99',
  ];
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return palette[(hash >>> 0) % palette.length];
}
