/** P4 产品工程退休后，桌面与 S3 的源码依赖必须保持闭合。 */
import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const pc = fileURLToPath(new URL("../", import.meta.url));
const repo = resolve(pc, "..");
const read = name => readFileSync(join(pc, name), "utf8");
function files(directory, extension) {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const path = join(directory, entry.name);
    return entry.isDirectory() ? files(path, extension) : path.endsWith(extension) ? [path] : [];
  });
}

test("已删除指定目录，保留 S3 工程、第一方许可与组件声明", () => {
  for (const path of ["firmware", "licenses", "pc/src-tauri/firmware/esp32-p4"]) {
    assert.equal(existsSync(join(repo, path)), false, path);
  }
  for (const path of ["LICENSE", "THIRD_PARTY_NOTICES.md", "firmware-s3/platformio.ini", "firmware-s3/src/main.cpp", "pc/builtin-clawpkgs/computer-status/licenses/sysinfo-LICENSE.txt"]) {
    assert.equal(existsSync(join(repo, path)), true, path);
  }
});

test("桌面准备与跨平台安装资源不再要求 P4 固件或出厂素材预处理", () => {
  const prepare = read("scripts/prepare-desktop-resources.mjs");
  const windows = read("scripts/build-windows-nsis-cross.mjs");
  assert.doesNotMatch(prepare, /prepareBuiltInP4Ready|validateBundledP4Firmware|p4ProjectCmake/);
  assert.doesNotMatch(windows, /bundledFirmwareSource|bundledFirmwareDestination/);
  for (const name of readdirSync(join(pc, "src-tauri")).filter(name => /^tauri(?:\.[\w-]+)?\.conf\.json$/.test(name))) {
    const config = JSON.parse(read(`src-tauri/${name}`));
    assert.doesNotMatch(JSON.stringify(config.bundle?.resources || {}), /esp32-p4|firmware-v3\.bin/);
  }
  const ci = readFileSync(join(repo, ".github/workflows/source-checks.yml"), "utf8");
  assert.doesNotMatch(ci, /firmware-host:|firmware\/tools|esp32_p4_evboard/);
  assert.match(ci, /firmware-s3:/);
});

test("内置组件目录与可选兼容素材工具只读取 PC 内部目录", () => {
  const config = JSON.parse(read("builtin-catalog.json"));
  assert.equal(new Set(config.components.ids).size, config.components.ids.length);
  for (const id of config.components.ids) {
    assert.equal(existsSync(join(pc, config.components.packagesDir, id, "component.json")), true, id);
  }
  assert.equal(existsSync(join(pc, config.appearance.clipsDir)), true);
  assert.match(read("src/component-center/library-order.js"), /\.\.\/\.\.\/builtin-catalog\.json/);
  assert.match(read("scripts/prepare-p4-ready-assets.mjs"), /join\(runtimeRoot, "builtin-catalog\.json"\)/);
});

test("所有 Rust 静态 include 文件均存在，不引用已删除的 P4 工程", () => {
  let checked = 0;
  for (const file of files(join(pc, "src-tauri/src"), ".rs")) {
    const source = readFileSync(file, "utf8");
    for (const match of source.matchAll(/include_(?:str|bytes)!\(\s*"([^"]+)"\s*\)/g)) {
      assert.equal(existsSync(resolve(dirname(file), match[1])), true, `${file}: ${match[1]}`);
      assert.doesNotMatch(match[1], /(?:^|\/)firmware\//);
      checked++;
    }
  }
  assert.ok(checked > 10, "必须实际检查静态契约与资源引用");
});

test("显式 WASM 重建使用 PC 纯核心，原有预览二进制继续存在", () => {
  for (const file of readdirSync(join(pc, "scripts")).filter(name => /^build-.*-runtime\.mjs$/.test(name))) {
    const source = read(`scripts/${file}`);
    assert.doesNotMatch(source, /firmware\/main/);
    for (const match of source.matchAll(/pc\/runtime-core\/([^'"]+\.[ch])/g)) {
      assert.equal(existsSync(join(pc, "runtime-core", match[1])), true, match[1]);
    }
  }
  for (const file of readdirSync(join(pc, "runtime-core")).filter(name => /\.[ch]$/.test(name))) {
    const source = read(`runtime-core/${file}`);
    for (const match of source.matchAll(/#include\s*"([^"]+)"/g)) {
      assert.equal(existsSync(join(pc, "runtime-core", match[1])), true, `${file}: ${match[1]}`);
    }
  }
  for (const name of ["classic-core.wasm", "program-core.wasm", "canvas-core.wasm", "form-v1-core.wasm"]) {
    const bytes = readFileSync(join(pc, "src/component-center", name));
    assert.equal(WebAssembly.validate(bytes), true, name);
  }
});

test("自动 P4 升级明确不可用，不从旧构建缓存选择固件", () => {
  const native = read("src-tauri/src/lib.rs");
  assert.doesNotMatch(native, /bundled_p4_firmware_path|bundled_resource_for_chip/);
  assert.match(native, /当前版本未提供内置固件/);
  const command = native.match(/async fn usb_update_bundled_firmware\([\s\S]*?\n\}/)?.[0];
  assert.ok(command);
  assert.match(command, /Err\(BUNDLED_P4_FIRMWARE_UNAVAILABLE/);
  assert.doesNotMatch(command, /run_usb_firmware_update|\.send\(|firmware_chip_target/);
  assert.match(native, /async fn usb_update_firmware\(/);
});
