/* global AbortSignal, fetch, setTimeout, setInterval */
import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { delimiter, dirname, resolve } from 'node:path';
import { fileURLToPath, URL } from 'node:url';

const frontendRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const worktreeRoot = resolve(frontendRoot, '..');
const windows = process.platform === 'win32';
const buildRoot = process.env.STREAMFIND_DEV_BUILD_DIR ||
  resolve(worktreeRoot, 'tmp', 'build', windows ? 'windows' : 'linux', 'core');
const executableSuffix = windows ? '.exe' : '';
const serviceExecutable = resolve(buildRoot, `streamfind-server${executableSuffix}`);
const workerExecutable = resolve(buildRoot, `streamfind-worker${executableSuffix}`);
const serviceUrl = (process.env.STREAMFIND_SERVICE_URL || 'http://127.0.0.1:8790').replace(/\/+$/, '');
const children = [];
let shuttingDown = false;

function log(message) {
  process.stdout.write(`[streamfind] ${message}\n`);
}

async function serviceReady() {
  try {
    const response = await fetch(`${serviceUrl}/session`, { signal: AbortSignal.timeout(1000) });
    if (!response.ok) return false;
    const session = await response.json();
    return session?.state === 'ready';
  } catch {
    return false;
  }
}

function startService(servicePort) {
  if (!existsSync(serviceExecutable)) {
    throw new Error(`Native service was not found. Build streamfind_service in: ${buildRoot}`);
  }
  if (!existsSync(workerExecutable)) {
    throw new Error(`Native worker was not found. Build streamfind_worker (packaged as streamfind-worker) in: ${buildRoot}`);
  }
  log(`Using native service binary: ${serviceExecutable}`);
  log(`Using matching operation worker: ${workerExecutable}`);
  const toolchainRoot = process.env.STREAMFIND_MSYS2_ROOT || 'C:/msys64';
  const pathEntries = windows
    ? [buildRoot, resolve(toolchainRoot, 'ucrt64/bin'), process.env.PATH || '']
    : [buildRoot, process.env.PATH || ''];
  const child = spawn(serviceExecutable, [servicePort], {
    cwd: buildRoot,
    env: { ...process.env, PATH: pathEntries.join(delimiter) },
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: false,
  });
  children.push(child);
  child.stdout.on('data', (chunk) => process.stdout.write(`[backend] ${chunk}`));
  child.stderr.on('data', (chunk) => process.stderr.write(`[backend] ${chunk}`));
  child.on('exit', (code) => {
    if (!shuttingDown) {
      log(`Native service exited with code ${code}.`);
      shutdown(code ?? 1);
    }
  });
  child.on('error', (error) => {
    console.error(`[streamfind] ${error.message}`);
    shutdown(1);
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
    env: { ...process.env, VITE_STREAMFIND_SERVICE_URL: serviceUrl },
    stdio: 'inherit',
    windowsHide: false,
  });
  children.push(child);
  child.on('exit', (code, signal) => {
    if (shuttingDown) return;
    const reason = signal ? `signal ${signal}` : `code ${code ?? 1}`;
    log(`Frontend exited with ${reason}; shutting down the native service.`);
    shutdown(code ?? 1);
  });
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
  const frontendOnly = process.argv.includes('--frontend-only');
  if (await serviceReady()) {
    log(`Using existing native service at ${serviceUrl}. It will stay running when this launcher exits.`);
  } else {
    if (frontendOnly) throw new Error(`No ready service at ${serviceUrl}. Start one before running the frontend.`);
    const endpoint = new URL(serviceUrl);
    if (endpoint.protocol !== 'http:' || endpoint.hostname !== '127.0.0.1' || !endpoint.port) {
      throw new Error('Starting a development service requires an http://127.0.0.1:<port> URL.');
    }
    log(`Starting native service on port ${endpoint.port}.`);
    startService(endpoint.port);
    await waitForService();
  }
  if (process.argv.includes('--backend-only')) {
    log(`Backend is ready at ${serviceUrl}. Press Ctrl+C to exit the launcher.`);
    if (!children.length) setInterval(() => {}, 60_000);
  } else {
    log('Starting Vite frontend at http://127.0.0.1:5173/.');
    startFrontend();
  }
} catch (error) {
  console.error(`[streamfind] ${error instanceof Error ? error.message : String(error)}`);
  shutdown(1);
}
