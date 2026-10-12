import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';

export async function discoverPluginManifests(pluginsRoot) {
  const pluginDirectories = await readdir(pluginsRoot, { withFileTypes: true });
  const registrations = [];
  for (const directory of pluginDirectories) {
    if (!directory.isDirectory()) continue;
    const manifestPath = join(pluginsRoot, directory.name, 'plugin.manifest.json');
    try {
      const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
      validatePluginManifest(manifest, manifestPath);
      if (manifest.activation === 'optional') registrations.push(manifest);
    } catch (error) {
      if (error?.code !== 'ENOENT') throw error;
    }
  }
  return registrations.sort((left, right) => left.id.localeCompare(right.id));
}

function validatePluginManifest(manifest, source) {
  if (!manifest || typeof manifest !== 'object') throw new Error(`Plugin manifest must be an object: ${source}`);
  for (const field of ['id', 'name', 'version', 'apiVersion', 'activation', 'source']) {
    if (typeof manifest[field] !== 'string' || !manifest[field].trim()) {
      throw new Error(`Plugin manifest field is required: ${source} (${field})`);
    }
  }
  if (!['bootstrap', 'optional'].includes(manifest.activation)) {
    throw new Error(`Unsupported plugin activation: ${source} (${manifest.activation})`);
  }
  for (const field of ['domains', 'artifactContracts', 'artifactRepresentations', 'visualizationTypes']) {
    if (
      manifest[field] !== undefined &&
      (!Array.isArray(manifest[field]) || manifest[field].some((item) => typeof item !== 'string'))
    ) {
      throw new Error(`Plugin manifest field must be a string array: ${source} (${field})`);
    }
  }
}
