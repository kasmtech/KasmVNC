// Build through Vite's API so the inventory follows rendered Rollup modules,
// including libraries incorrectly classified as devDependencies upstream.
import { build } from 'vite';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';

const bundled = new Set();
const evidence = new Map();
const root = process.cwd();
const inventoryPlugin = () => ({
  name: 'kasm-shipped-module-inventory',
  generateBundle(_options, bundle) {
    for (const chunk of Object.values(bundle)) {
      if (chunk.type !== 'chunk') continue;
      for (const [id, info] of Object.entries(chunk.modules)) {
        if (info.renderedLength === 0 || !id.includes('/node_modules/')) continue;
        const file = id.replace(/^\0/, '').split('?')[0];
        let dir = path.dirname(file);
        while (dir.startsWith(root) && !fs.existsSync(path.join(dir, 'package.json'))) dir = path.dirname(dir);
        if (!dir.startsWith(root) || !dir.includes('/node_modules/')) throw new Error(`No package owner: ${id}`);
        bundled.add(dir);
        if (!evidence.has(dir)) evidence.set(dir, new Set());
        evidence.get(dir).add(path.relative(root, file));
      }
    }
  }
});
await build({ plugins: [inventoryPlugin()], worker: { plugins: () => [inventoryPlugin()] } });
// Preserve the extra assets copied by the upstream npm build command.
fs.mkdirSync('dist/app', { recursive: true });
fs.cpSync('app/locale', 'dist/app/locale', { recursive: true });
fs.copyFileSync('package.json', 'dist/package.json');
fs.mkdirSync('dist/core/decoders/qoi', { recursive: true });
fs.copyFileSync('core/decoders/qoi/qoi_viewer_bg.wasm', 'dist/core/decoders/qoi/qoi_viewer_bg.wasm');
fs.copyFileSync('dist/index.html', 'dist/vnc.html');
const staging = '/tmp/frontend-sources';
fs.mkdirSync(staging, { recursive: true });
fs.copyFileSync('/src/www-source.tar.gz', `${staging}/novnc-source.tar.gz`);
fs.copyFileSync('package-lock.json', `${staging}/package-lock.json`);
fs.copyFileSync('.kasm-supply-chain.mjs', `${staging}/build.mjs`);
const lock = JSON.parse(fs.readFileSync('package-lock.json'));
const components = [];
for (const dir of [...bundled].sort()) {
  const pkg = JSON.parse(fs.readFileSync(path.join(dir, 'package.json')));
  if (!pkg.name || !pkg.version) throw new Error(`Incomplete bundled package ${dir}`);
  const relative = path.relative(root, dir);
  const entry = lock.packages[relative];
  if (!entry?.integrity) throw new Error(`Missing locked integrity for ${relative}`);
  const dest = path.join(staging, relative);
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.cpSync(dir, dest, { recursive: true, filter: p => !path.relative(dir, p).split(path.sep).some(part => ['node_modules', '.git', '.env', '.npmrc'].includes(part)) });
  components.push({ name: pkg.name, version: pkg.version, license: pkg.license || 'NOASSERTION',
    purl: `pkg:npm/${pkg.name.replace('@', '%40')}@${pkg.version}`,
    source: relative, resolved: entry.resolved, integrity: entry.integrity,
    modules: [...evidence.get(dir)].sort() });
}
const sha256 = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const assets = {};
function walk(dir) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const file = path.join(dir, ent.name);
    if (ent.isDirectory()) walk(file);
    else if (ent.isFile()) assets[path.relative('dist', file)] = sha256(file);
    else throw new Error(`Unexpected frontend symlink: ${file}`);
  }
}
walk('dist');
fs.mkdirSync('/supply-chain', { recursive: true });
execFileSync('tar', ['-czf', '/supply-chain/sources.tar.gz', '-C', staging, '.']);
const pkg = JSON.parse(fs.readFileSync('package.json'));
const manifest = { format: 'frontend-provenance-v1', name: pkg.name, version: pkg.version,
  revision: fs.readFileSync('/src/www-source-revision', 'utf8').trim(),
  repository: 'https://github.com/kasmtech/noVNC', license: pkg.license,
  components, assets, source_archive: 'sources.tar.gz', source_sha256: sha256('/supply-chain/sources.tar.gz'),
  limitations: ['The vendored QOI WebAssembly binary is preserved by hash in the noVNC snapshot; its original Rust source and dependency lockfile are not present in this repository. Complete corresponding-source verification for that pre-existing binary remains outstanding.',
    'Installed npm package contents and the generated lockfile are preserved for rendered modules. Build-only npm dependencies are recorded in the lockfile, not classified as shipped components. Vendored frontend code remains part of the noVNC source snapshot.'] };
fs.writeFileSync('/supply-chain/manifest.json', JSON.stringify(manifest, null, 2) + '\n');
console.log(`Recorded ${components.length} bundled npm packages and ${Object.keys(assets).length} frontend assets`);
