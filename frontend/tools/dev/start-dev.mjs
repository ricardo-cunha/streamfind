/* global fetch, setTimeout */
import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, URL } from 'node:url';

const frontendRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const worktreeRoot = resolve(frontendRoot, '..');
const buildRoot = resolve(worktreeRoot, 'tmp', 'build', 'mingw-ucrt64');
const serviceExecutable = resolve(buildRoot, 'streamfind_service.exe');
const serviceUrl = process.env.STREAMFIND_SERVICE_URL || 'http://127.0.0.1:8790';
const servicePort = new URL(serviceUrl).port || '8790';
const children = [];
let shuttingDown = false;

function log(message) {
  process.stdout.write(`[streamfind] ${message}\n`);
}

async function serviceReady() {
  try {
    const response = await fetch(`${serviceUrl}/session`);
    if (!response.ok) return false;
    const session = await response.json();
    return session?.state === 'ready';
  } catch {
    return false;
  }
}

function startService() {
  if (!existsSync(serviceExecutable)) {
    throw new Error(`Native service was not found at ${serviceExecutable}. Build streamfind_service first.`);
  }
  const pathEntries = [buildRoot, 'C:/msys64/ucrt64/bin', 'C:/msys64/usr/bin', process.env.PATH || ''];
  const child = spawn(serviceExecutable, [servicePort], {
    cwd: buildRoot,
    env: { ...process.env, PATH: pathEntries.join(';') },
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: false,
  });
  children.push(child);
  child.stdout.on('data', (chunk) => process.stdout.write(`[backend] ${chunk}`));
  child.stderr.on('data', (chunk) => process.stderr.write(`[backend] ${chunk}`));
  child.on('exit', (code) => {
    if (!shuttingDown && code !== 0) log(`Native service exited with code ${code}.`);
  });
}

async function waitForService(timeoutMs = 30_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await serviceReady()) return;
    await new Promise((resolvePromise) => setTimeout(resolvePromise, 250));
  }
  throw new Error(`Native service did not become ready at ${serviceUrl}.`);
}

function startFrontend() {
  const viteBin = resolve(frontendRoot, 'node_modules', 'vite', 'bin', 'vite.js');
  const child = spawn(process.execPath, [viteBin, '--host', '127.0.0.1'], {
    cwd: frontendRoot,
    env: process.env,
    stdio: 'inherit',
    windowsHide: false,
  });
  children.push(child);
}

function shutdown(exitCode = 0) {
  if (shuttingDown) return;
  shuttingDown = true;
  for (const child of children.reverse()) {
    if (process.platform === 'win32' && child.pid) {
      spawnSync('taskkill.exe', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
    }
    if (!child.killed) child.kill();
  }
  setTimeout(() => process.exit(exitCode), 100);
}

process.on('SIGINT', () => shutdown());
process.on('SIGTERM', () => shutdown());

try {
  if (await serviceReady()) {
    log(`Using existing native service at ${serviceUrl}.`);
  } else {
    log(`Starting native service on port ${servicePort}.`);
    startService();
    await waitForService();
  }
  if (process.argv.includes('--backend-only')) {
    log(`Backend is ready at ${serviceUrl}. Press Ctrl+C to stop it.`);
    await new Promise(() => {});
  } else {
    log('Starting Vite frontend at http://127.0.0.1:5173/.');
    startFrontend();
  }
} catch (error) {
  console.error(`[streamfind] ${error instanceof Error ? error.message : String(error)}`);
  shutdown(1);
}
