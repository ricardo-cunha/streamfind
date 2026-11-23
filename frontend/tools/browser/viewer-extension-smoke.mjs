import { chromium } from 'playwright';

const baseUrl = process.env.STREAMFIND_FRONTEND_URL || 'http://127.0.0.1:8789/';
const expectedContract =
  process.env.STREAMFIND_VIEWER_CONTRACT || 'https://streamfind.dev/catalogue/plugins/alpha#result';
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
const errors = [];

page.on('pageerror', (error) => errors.push(error.message));
page.on('console', (message) => {
  if (message.type() === 'error') errors.push(message.text());
});

try {
  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => Boolean(globalThis.__streamfindReact && globalThis.__streamfindReactDomClient));

  const discovery = await page.evaluate(async (contract) => {
    const manifest = await fetch('/plugins.json').then((response) => response.json());
    const viewerManifest = manifest.plugins.find(
      (plugin) => plugin.kind === 'viewer' && plugin.artifactContracts?.includes(contract),
    );
    if (!viewerManifest) throw new Error(`qualified viewer contract was not discovered: ${contract}`);

    const module = await import(viewerManifest.entry);
    const plugin = module.default ?? module.plugin;
    if (!plugin) throw new Error('viewer bundle did not export a plugin');

    const registrations = [];
    plugin.setup({
      registerViewer(viewer) {
        registrations.push(viewer);
      },
    });
    const registration = registrations.find((viewer) => viewer.accepts.includes(contract));
    if (!registration) throw new Error('viewer did not register the discovered qualified contract');

    const sdk = await import('/extensions/runtime/viewer-sdk.mjs');
    const collisionContract = contract.replace('/alpha#', '/beta#');
    if (!sdk.artifactContractMatches(contract, contract)) throw new Error('exact contract did not match');
    if (sdk.artifactContractMatches(contract, collisionContract)) {
      throw new Error('same-local-name contract incorrectly matched');
    }

    const host = document.createElement('div');
    host.id = 'phase8-viewer-root';
    document.body.append(host);
    const react = globalThis.__streamfindReact;
    const root = globalThis.__streamfindReactDomClient.createRoot(host);
    root.render(
      react.createElement(registration.component, {
        context: { resultId: 'phase8-result', semanticType: contract },
      }),
    );
    await new Promise((resolve) => setTimeout(resolve, 50));
    const button = host.querySelector('button');
    if (!button) throw new Error('viewer did not render a component button');
    return {
      id: viewerManifest.id,
      entry: viewerManifest.entry,
      digest: viewerManifest.packageDigest,
      acceptedContract: registration.accepts[0],
      assetUrl: button.getAttribute('data-asset-url'),
    };
  }, expectedContract);

  const root = page.locator('#phase8-viewer-root');
  await root.getByRole('button', { name: 'Select phase8-result' }).click();
  await root.getByRole('button', { name: 'Selected phase8-result' }).waitFor();

  const assetUrl = new URL(discovery.assetUrl, baseUrl).toString();
  const assetResponse = await page.request.get(assetUrl);
  if (!assetResponse.ok()) throw new Error(`viewer asset failed: ${assetResponse.status()} ${assetUrl}`);
  const stylesheet = page.locator(`link[data-streamfind-plugin="${discovery.id}"]`);
  await stylesheet.waitFor({ state: 'attached' });
  if (errors.length) throw new Error(`browser errors: ${errors.join(' | ')}`);
  console.log(JSON.stringify({ ...discovery, assetStatus: assetResponse.status(), statefulClick: true }));
} finally {
  await browser.close();
}
