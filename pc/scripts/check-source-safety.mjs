/** [Input] Public tracked and unignored files. [Output] A fail-closed credential/artifact gate.
 * [Pos] Public source CI; complementary to Gitleaks, not a full security audit. */
import { execFileSync } from 'node:child_process';
import { readFileSync, lstatSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';
import { createHash } from 'node:crypto';
// Content digests, not Git object IDs or private image data.
const BLOCKED_PUBLICATION_MEDIA = new Set([
  'dba713d4f929eb69e7537fcac84bc4ff92dde2eb89f55bfa32ed999e30c0f746',
  '6eec1e03174f797057e27074a67569cf64b82423cf80405d4109888af88f6ffd',
]);
// This public factory image contains mbedTLS' empty PEM marker strings in its
// read-only error table. Permit only this exact reviewed image to avoid treating
// marker text as a private key; credential-shaped values remain blocked.
const APPROVED_FIRMWARE_MARKER_IMAGES = new Map([
  ['firmware-s3/releases/firmware.factory.bin', '4728c485259a104b0f0f8dab0ba862d63054f8c9e9952cb0c6d9ce28dee84aaf'],
]);
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const paths = execFileSync('git', ['ls-files', '--cached', '--others', '--exclude-standard', '-z'], {cwd: root})
  .toString().split('\0').filter(Boolean);
const problems = [];
for (const file of new Set(paths)) {
  const absolute = resolve(root, file);
  // Git 索引会包含工作树中已删除但尚未暂存的文件；这些文件不参与分发。
  const stat = lstatSync(absolute, { throwIfNoEntry: false });
  if (!stat) continue;
  // These two hand-written fixtures emulate the upstream package layout, not build output.
  const testFixture = /^pc\/src-tauri\/bridge\/packages\/agent-session-bus\/test\/fixtures\/fake-openclaw\/dist\/(?:index|plugin-sdk\/agent-runtime)\.js$/.test(file);
  if (!testFixture && /(?:^|\/)(?:node_modules|dist|target|generated-runtime|\.runtime-cache|\.vite|\.pio|managed_components|legacy|\.claude|\.codex)(?:\/|$)/.test(file)
      || /(?:\.internal-build-|bundle-secrets\.env|\.p12$|\.pfx$|\.pem$|\.key$|\.log$|\.dmg$|\.exe$|\.msi$|\.jsonl$)/i.test(file)
      || /(?:^|\/)\.env(?:\.|$)/.test(file) && !file.endsWith('.env.example')) problems.push(`${file}: forbidden publication input`);
  if (stat.isSymbolicLink()) { problems.push(`${file}: symlink requires review`); continue; }
  const bytes = readFileSync(absolute);
  if (/\.(?:png|jpe?g|webp|gif)$/i.test(file) && BLOCKED_PUBLICATION_MEDIA.has(createHash('sha256').update(bytes).digest('hex'))) {
    problems.push(`${file}: previously removed unredacted media`);
  }
  // Scan ASCII credential markers in binary assets as well as source text.
  // Null bytes in a generated firmware image must not bypass this check.
  const text = bytes.toString();
  const approvedFirmwareMarkerImage = APPROVED_FIRMWARE_MARKER_IMAGES.get(file) === sha256(bytes);
  const hasPrivateKeyMarker = /-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----/.test(text);
  const hasCredentialToken = /\b(?:ark-[a-f0-9-]{25,}|sk-[A-Za-z0-9_-]{24,}|gh[pousr]_[A-Za-z0-9]{24,}|AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|glpat-[A-Za-z0-9_-]{20,}|AIza[0-9A-Za-z_-]{30,})/.test(text);
  if ((hasPrivateKeyMarker && !approvedFirmwareMarkerImage) || hasCredentialToken) {
    problems.push(`${file}: potential credential; value redacted`);
  }
  if (text.includes(`${homedir()}/`)) problems.push(`${file}: personal build path`);
  if (/\bgit\.n\.[a-z0-9.-]+\b/i.test(text)) problems.push(`${file}: private repository endpoint`);
  // Text-only policy checks do not apply to resolved binary assets.
  if (bytes.includes(0)) continue;
  if ((/^pc\/docs\//.test(file) || /^(?:README(?:\.en)?\.md|firmware\/README\.md)$/.test(file)) && /\.md$/.test(file)
      && /内部版本有意保留|保留明确授权的内部凭据\/CA|内部版批准的凭据\/CA|本机正式库已移动到私有|本机证据在工作区同级|内部 Windows 包：|当前内部构建|两项已授权 Key|两张企业 CA|仅限内部流转/.test(text)) {
    problems.push(`${file}: private delivery or local acceptance notes`);
  }
  if (text.startsWith('version https://git-lfs.github.com/spec/v1\n')) {
    problems.push(`${file}: unresolved LFS pointer; public resources must be available in ordinary Git`);
  }
}
if (problems.length) { console.error(problems.join('\n')); process.exit(1); }
console.log(`Source publication checks passed for ${paths.length} paths.`);
