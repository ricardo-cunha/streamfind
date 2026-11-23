import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';

export async function discoverPluginManifests(pluginsRoot, { includeBootstrap = false } = {}) {
  const pluginDirectories = [];
  async function collect(directory, relative = '') {
    for (const entry of await readdir(directory, { withFileTypes: true })) {
      const entryRelative = join(relative, entry.name);
      if (entry.isDirectory()) await collect(join(directory, entry.name), entryRelative);
      else if (entry.name === 'plugin.manifest.json') pluginDirectories.push({ directory, relative: entryRelative });
    }
  }
  await collect(pluginsRoot);
  const registrations = [];
  for (const entry of pluginDirectories) {
    const manifestPath = join(entry.directory, 'plugin.manifest.json');
    try {
      const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
      validatePluginManifest(manifest, manifestPath);
      if (manifest.activation === 'optional' || (includeBootstrap && manifest.activation === 'bootstrap')) {
        registrations.push(manifest);
      }
    } catch (error) {
      if (error?.code !== 'ENOENT') throw error;
    }
  }
  return registrations.sort((left, right) => left.id.localeCompare(right.id));
}

function validatePluginManifest(manifest, source) {
  if (!manifest || typeof manifest !== 'object') throw new Error(`Plugin manifest must be an object: ${source}`);
  if (manifest.schemaVersion !== 1) {
    throw new Error(`Unsupported plugin manifest schema: ${source} (${manifest.schemaVersion})`);
  }
  if (manifest.kind !== 'viewer') {
    throw new Error(`Unsupported plugin manifest kind: ${source} (${manifest.kind})`);
  }
  for (const field of ['id', 'name', 'version', 'apiVersion', 'activation', 'source']) {
    if (typeof manifest[field] !== 'string' || !manifest[field].trim()) {
      throw new Error(`Plugin manifest field is required: ${source} (${field})`);
    }
  }
  if (!['bootstrap', 'optional'].includes(manifest.activation)) {
    throw new Error(`Unsupported plugin activation: ${source} (${manifest.activation})`);
  }
  for (const field of ['plugins', 'artifactContracts', 'artifactRepresentations', 'visualizationTypes']) {
    if (
      manifest[field] !== undefined &&
      (!Array.isArray(manifest[field]) || manifest[field].some((item) => typeof item !== 'string'))
    ) {
      throw new Error(`Plugin manifest field must be a string array: ${source} (${field})`);
    }
  }
  if (manifest.artifactContracts?.some((contract) => !isQualifiedContract(contract))) {
    throw new Error(`Plugin manifest artifact contracts must be qualified IRIs: ${source}`);
  }
}

function isQualifiedContract(value) {
  return value.includes('#') || /^[A-Za-z][A-Za-z0-9+.-]*:\S+$/.test(value);
}
