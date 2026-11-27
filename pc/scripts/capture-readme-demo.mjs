/**
 * 独立 HTML → 仅含五屏键盘的 README GIF；无编译、服务或设备访问。
 * 用法：node pc/scripts/capture-readme-demo.mjs [--keep-frames]
 * 屏幕 1 使用用户指定的 working.mp4，从第一帧直接播放，不展示启动文字。
 */
import assert from 'node:assert/strict';
import { access, mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { constants } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, delimiter, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { chromium } from 'playwright';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const assets = join(root, 'assets');
const keepFrames = process.argv.includes('--keep-frames');
assert.ok(process.argv.slice(2).every(arg => arg === '--keep-frames'), '仅支持 --keep-frames');

async function existing(candidates) {
  for (const candidate of candidates.filter(Boolean)) {
    try { await access(candidate, constants.X_OK); return candidate; } catch {}
  }
  return null;
}
function onPath(names) {
  return (process.env.PATH || '').split(delimiter).flatMap(dir => names.map(name => join(dir, name)));
}
const browserPath = await existing([
  process.env.PET_TEST_BROWSER,
  ...(process.platform === 'win32' ? [
    process.env['ProgramFiles(x86)'] && join(process.env['ProgramFiles(x86)'], 'Microsoft/Edge/Application/msedge.exe'),
    process.env.ProgramFiles && join(process.env.ProgramFiles, 'Microsoft/Edge/Application/msedge.exe'),
  ] : process.platform === 'darwin' ? [
    '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ] : onPath(['chromium', 'chromium-browser', 'google-chrome'])),
  chromium.executablePath(),
]);
assert.ok(browserPath, '未找到已有浏览器。请用 PET_TEST_BROWSER 指定本机浏览器；本脚本不会下载。');
const ffmpeg = await existing([
  process.env.PET_TEST_FFMPEG,
  join(root, 'pc/src-tauri/generated-runtime', process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg'),
  ...onPath([process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg']),
]);
assert.ok(ffmpeg, '未找到已有 FFmpeg。请用 PET_TEST_FFMPEG 指定本机工具；本脚本不会下载。');
const avatarSource = join(root, 'pc/public/builtin-appearances/c867abad-fabb-4704-b6ee-22d6fc0052fd/videos/working.mp4');
await access(avatarSource, constants.F_OK);
const html = await readFile(join(root, 'pc/tests/readme-demo.html'), 'utf8');
const tempRoot = process.env.CLAUDE_JOB_DIR ? join(process.env.CLAUDE_JOB_DIR, 'tmp') : tmpdir();
await mkdir(tempRoot, { recursive: true });
const frames = await mkdtemp(join(tempRoot, 'easycoding-readme-'));
const errors = [], blocked = [];
// 指定视频为 10 fps、0.8 秒；按原帧率循环八次，不改变角色动作节奏。
const fps = 10;
const duration = 6.4;
let browser;

function encode(args, label) {
  const result = spawnSync(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin', '-y', ...args], {
    cwd: root, encoding: 'utf8', timeout: 120000, maxBuffer: 2 * 1024 * 1024, windowsHide: true,
  });
  assert.equal(result.status, 0, `${label}失败；旧输出未更改：${result.error?.message || result.stderr}`);
}

try {
  const avatarGifPath = join(frames, 'avatar.gif');
  encode(['-i', avatarSource, '-filter_complex',
    `fps=${fps},scale=120:120:force_original_aspect_ratio=decrease:flags=neighbor,pad=120:120:(ow-iw)/2:(oh-ih)/2:black,split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=none`,
    '-loop', '0', avatarGifPath], '指定动画 GIF 转换');
  const avatarGif = await readFile(avatarGifPath);
  assert.match(avatarGif.subarray(0, 6).toString('ascii'), /^GIF8[79]a$/);
  const demoHtml = html.replaceAll('__AVATAR_GIF_DATA__', `data:image/gif;base64,${avatarGif.toString('base64')}`);
  assert.doesNotMatch(demoHtml, /__AVATAR_GIF_DATA__|HACHIMI|AVATAR|初始化|黑屏/);
  // 确定性解码 GIF 帧后截图，避免依赖浏览器墙钟导致卡顿或动画重启。
  encode(['-i', avatarGifPath, '-vf', `fps=${fps}`, join(frames, 'avatar-%03d.png')], '动画帧解码');
  const avatarFiles = (await readdir(frames)).filter(name => /^avatar-\d{3}\.png$/.test(name)).sort();
  assert.ok(avatarFiles.length > 1, '指定视频必须包含多个真实动画帧');
  const avatarFrames = await Promise.all(avatarFiles.map(async name => {
    const bytes = await readFile(join(frames, name));
    return `data:image/png;base64,${bytes.toString('base64')}`;
  }));
  assert.ok(new Set(avatarFrames).size > 1, '指定视频不能退化为重复静态帧');

  browser = await chromium.launch({ executablePath: browserPath, headless: true });
  const context = await browser.newContext({
    viewport: { width: 1000, height: 600 }, deviceScaleFactor: 1,
    locale: 'zh-CN', timezoneId: 'UTC', reducedMotion: 'reduce', serviceWorkers: 'block',
  });
  await context.route('**/*', route => { blocked.push('HTTP'); return route.abort('blockedbyclient'); });
  await context.routeWebSocket('**/*', socket => { blocked.push('WebSocket'); socket.close(); });
  const page = await context.newPage();
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
  page.setDefaultTimeout(10000);
  await page.setContent(demoHtml, { waitUntil: 'load' });
  await page.evaluate(() => document.fonts.ready);
  await page.waitForFunction(() => window.readmeDemo?.ready === true);
  const scene = page.locator('#scene');
  const setAvatarFrame = src => page.evaluate(async src => {
    const image = document.querySelector('[data-screen="1"] img');
    image.src = src;
    await image.decode();
  }, src);

  assert.equal(await page.locator('.screen').count(), 5);
  assert.equal(await page.locator('.window, .sidebar, .workspace').count(), 0, '导出内容不得包含桌面应用区域');
  assert.equal(await page.locator('.screen img').count(), 1, '只有屏幕 1 播放指定动画');
  assert.match(await page.locator('.screen img').getAttribute('src'), /^data:image\/gif;base64,/);
  assert.match(await page.locator('[data-screen="2"]').textContent(), /SENDKEY 2/);
  assert.match(await page.locator('[data-screen="4"]').textContent(), /CANCELKEY 4/);
  assert.match(await page.locator('.disclaimer').textContent(), /非实机录像/);
  assert.match(await page.locator('.disclaimer').textContent(), /Not a hardware recording/);
  const overflow = await scene.evaluate(root => [root, ...root.querySelectorAll('*')]
    .filter(el => el.clientWidth > 0 && (el.scrollWidth > el.clientWidth + 1 || el.scrollHeight > el.clientHeight + 1))
    .map(el => el.className || el.id));
  assert.deepEqual(overflow, [], '画面存在文字或容器溢出');

  const frameHashes = new Set();
  for (let index = 0; index < Math.round(duration * fps); index++) {
    // 第 0 帧即注入指定动画的首帧，没有黑屏或初始化文字前导。
    await setAvatarFrame(avatarFrames[index % avatarFrames.length]);
    assert.doesNotMatch(await scene.textContent(), /HACHIMI|AVATAR|初始化|黑屏/);
    const bytes = await scene.screenshot({ path: join(frames, `frame-${String(index).padStart(3, '0')}.png`) });
    frameHashes.add(createHash('sha256').update(bytes).digest('hex'));
  }
  assert.ok(frameHashes.size > 2, '导出帧必须显示指定视频的不同动作');
  assert.deepEqual(errors, [], '模拟页面不得有脚本或资源错误');
  assert.deepEqual(blocked, [], '模拟页面不得尝试业务网络连接');
  const gifPath = join(frames, 'preview.gif');
  encode(['-framerate', String(fps), '-i', join(frames, 'frame-%03d.png'),
    '-filter_complex', 'split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle',
    '-t', String(duration), '-loop', '0', gifPath], '设备 GIF 编码');
  const gif = await readFile(gifPath);
  assert.match(gif.subarray(0, 6).toString('ascii'), /^GIF8[79]a$/);
  assert.equal(gif.readUInt16LE(6), 1000);
  assert.equal(gif.readUInt16LE(8), 510);
  assert.ok(gif.length > 1000);
  await mkdir(assets, { recursive: true });
  await writeFile(join(assets, 'readme-startup.gif'), gif);
  if (keepFrames) await writeFile(join(frames, 'demo.html'), demoHtml);
  console.log(`GIF 已生成：assets/readme-startup.gif（${gif.length} bytes，1000×510，${duration} 秒）`);
  console.log(`屏幕 1 使用指定 working.mp4：${avatarFrames.length} 个源帧，${frameHashes.size} 个不同画面，从第 0 帧开始播放。`);
  console.log('验证通过：无初始化文字、仅键盘、五屏与 K2/K4 映射、无溢出、零 HTTP/WebSocket 请求、零页面错误。');
} finally {
  await browser?.close();
  if (keepFrames) console.log(`临时帧保留供目视检查：${frames}`);
  else await rm(frames, { recursive: true, force: true });
}
