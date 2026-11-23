/** 桌面资源不读取仓库根许可证目录；组件声明与独立运行资源校验继续保留。 */
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";

const pcRoot = fileURLToPath(new URL("../", import.meta.url));
const tauriRoot = join(pcRoot, "src-tauri");
const rootLicenses = resolve(pcRoot, "..", "licenses");
const read = name => readFileSync(join(pcRoot, name), "utf8");

for (const name of readdirSync(tauriRoot).filter(name => /^tauri(?:\.[\w-]+)?\.conf\.json$/.test(name))) {
  test(`${name} 不依赖根许可证目录或生成的 Node 许可证`, () => {
    const config = JSON.parse(read(`src-tauri/${name}`));
    const resources = config.bundle?.resources || {};
    const entries = Array.isArray(resources) ? resources.map(source => [source, source]) : Object.entries(resources);
    for (const [source, destination] of entries) {
      const absolute = resolve(tauriRoot, source);
      assert.ok(absolute !== rootLicenses && !absolute.startsWith(rootLicenses + sep), source);
      assert.doesNotMatch(`${source}\n${destination}`, /(?:^|[/\\])node\.LICENSE\b/);
    }
  });
}

for (const name of [
  "prepare-desktop-resources.mjs",
  "build-windows-nsis-cross.mjs",
  "prepare-official-community.mjs",
  "prepare-community-refresh.mjs",
]) {
  test(`${name} 不再校验或复制根许可证清单`, () => {
    const source = read(`scripts/${name}`);
    assert.doesNotMatch(source, /dependency-license-resources|verifyDesktopLicenseInventory|copyNodeRuntimeLicense|copyDependencyLicenses|dependencyLicenseResources|node\.LICENSE/);
    assert.doesNotMatch(source, /(?:join|resolve)\(\s*(?:repositoryRoot|repo)\s*,\s*["']licenses(?:[/\\]|["'])/);
  });
}

test("解除目录依赖不移除 Node 架构、bridge 依赖与 FFmpeg 来源校验", () => {
  const prepare = read("scripts/prepare-desktop-resources.mjs");
  assert.match(prepare, /assertRuntimeMatchesTarget\(sourceRuntime, target, targetArch\)/);
  assert.match(prepare, /ensureBridgeDependencies\(\)/);
  assert.match(prepare, /assertLgplFfmpegMetadata\(targetReadme, targetLicense, target\)/);
  assert.match(prepare, /const actualBinaryHash = sha256\(readFileSync\(targetRuntime\)\)/);
  assert.match(prepare, /actualBinaryHash !== expectedBinaryHash/);
  for (const flag of ["--disable-gpl", "--disable-nonfree", "--enable-gpl", "--enable-nonfree"]) {
    assert.ok(prepare.includes(flag), flag);
  }
  const windows = read("scripts/build-windows-nsis-cross.mjs");
  for (const name of ["ffmpeg.LICENSE", "ffmpeg.README", "ffmpeg.SOURCE.txt", "zlib.LICENSE", "ffmpeg-8.1.2.tar.xz"]) {
    assert.ok(windows.includes(name), name);
  }
  for (const platform of ["macos", "windows"]) {
    const { bundle: { resources } } = JSON.parse(read(`src-tauri/tauri.${platform}.conf.json`));
    assert.equal(resources["generated-runtime/ffmpeg.LICENSE"], "tools/ffmpeg.LICENSE");
    assert.equal(resources["generated-runtime/ffmpeg-8.1.2.tar.xz"], "tools/source/ffmpeg-8.1.2.tar.xz");
  }
});

for (const name of ["prepare-official-community.mjs", "prepare-community-refresh.mjs"]) {
  test(`${name} 保留组件自己的许可证文件白名单`, () => {
    const source = read(`scripts/${name}`);
    const declaration = source.match(/const allowed\s*=([^;]+);/);
    assert.ok(declaration, "必须存在组件文件白名单");
    // 只执行本地源码中的纯白名单表达式，不启动浏览器、联网或打包。
    const allowed = runInNewContext(`(${declaration[1]})`, {}, { timeout: 1000 });
    for (const path of ["LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "licenses/font-OFL.txt", "licenses/source.md"]) {
      assert.equal(allowed(path), true, path);
    }
    for (const path of ["licenses/../private.txt", "licenses/run.js", "licenses/font.pdf"]) {
      assert.equal(allowed(path), false, path);
    }
  });
}

test("官方社区 staging 不覆盖组件自带的版权声明", () => {
  const source = read("scripts/prepare-official-community.mjs");
  assert.match(source, /if\(!fs\.existsSync\(path\.join\(stage,targetName\)\)\)fs\.copyFileSync/);
});
