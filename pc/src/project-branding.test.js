/** 对外品牌、当前目录说明和主要文档链接的非编译契约。 */
import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../../", import.meta.url));
const read = path => readFileSync(resolve(root, path), "utf8");
const guides = [
  "README.md",
  "README.en.md",
  "pc/README.md",
  "firmware-s3/README.md",
  "pc/runtime-core/README.md",
  "pc/docs/runtime-dependencies.md",
  "docs/device-architecture.md",
  "pc/docs/windows-acceptance.md",
  "pc/docs/realtime-chat.md",
  "pc/docs/realtime-persona-voice-chat-design.md",
];

for (const path of guides) {
  test(`${path} 使用当前品牌和功能说明`, () => {
    const text = read(path);
    assert.ok(text.includes("EasyCoding Deck"), "文档应明确当前产品名称");
    // 第三方致谢中的 HachimoDock 是来源与版权信息，不等同于当前产品名称。
    const brandingText = text.replace(/HachimoDock 项目/g, "上游项目");
    assert.doesNotMatch(brandingText, /\bP4\b|ESP32-P4|HachimoDock(?:-main)?|HachimiDock|哈基米机/);
    assert.doesNotMatch(text, /已移除|已删除|原根目录|旧平台|原工程|迁移方案/);
  });
  test(`${path} 的本地 Markdown 链接均有目标`, () => {
    const text = read(path);
    for (const match of text.matchAll(/\[[^\]\n]*\]\(([^\s)]+)(?:\s+"[^"]*")?\)/g)) {
      const target = match[1].split(/[?#]/)[0];
      if (!target || /^[A-Za-z][A-Za-z\d+.-]*:/.test(target)) continue;
      const absolute = resolve(dirname(resolve(root, path)), decodeURIComponent(target));
      assert.equal(existsSync(absolute), true, `${path}: ${target}`);
    }
  });
}

test("应用、诊断页和智能家居文案品牌一致", () => {
  const config = JSON.parse(read("pc/src-tauri/tauri.conf.json"));
  assert.equal(config.productName, "EasyCoding Deck");
  assert.equal(config.app.windows[0].title, "EasyCoding Deck");
  assert.match(read("pc/index.html"), /<title>EasyCoding Deck<\/title>/);
  assert.match(read("pc/composer-diagnostic.html"), /<title>EasyCoding Deck · 输入框结构检查<\/title>/);
  const page = read("pc/src/SmartHome.jsx");
  assert.ok(page.includes("EasyCoding Deck"));
  assert.doesNotMatch(page, /哈基米|HachimoDock|HachimiDock/);
  assert.match(read("pc/src/dashboard/DashboardActionsMenu.jsx"), /手动升级固件/);
  assert.match(read("server/community/web/index.html"), /<title>EasyCoding Deck · 社区广场<\/title>/);
  assert.doesNotMatch(read("server/community/web/app.js"), /Pet Manager|哈基米专属/);
  assert.doesNotMatch(read("pc/src/lib/locale.js"), /Desktop Pet Manager/);
});

test("DSH 集成源码、打包资源与原生嵌入指向当前目录", () => {
  const integration = "integrations/dsh-easycoding-deck";
  assert.equal(existsSync(resolve(root, integration, "package.json")), true);
  const config = JSON.parse(read("pc/src-tauri/tauri.conf.json"));
  const resources = Object.entries(config.bundle.resources)
    .filter(([, destination]) => destination.startsWith("dsh-bridge/"));
  assert.ok(resources.length > 0, "必须实际检查 DSH 打包资源");
  for (const [source] of resources) {
    assert.ok(source.startsWith(`../../${integration}/`), source);
    assert.equal(existsSync(resolve(root, "pc/src-tauri", source)), true, source);
  }
  assert.ok(read("pc/src-tauri/src/dsh_windows_acl.rs")
    .includes(`include_str!("../../../${integration}/windows-acl.ps1")`));
});

test("内置组件的物理目录不暴露 hachimi 前缀，组件 ID 仍保持兼容", () => {
  const catalog = JSON.parse(read("pc/builtin-catalog.json"));
  const packageDirs = catalog.components.packageDirs || {};
  for (const [id, directory] of Object.entries(packageDirs)) {
    assert.doesNotMatch(directory, /hachimi/i, directory);
    assert.equal(existsSync(resolve(root, "pc/builtin-clawpkgs", directory, "component.json")), true, directory);
    const manifest = JSON.parse(read(`pc/builtin-clawpkgs/${directory}/component.json`));
    assert.equal(manifest.id, id);
  }
  assert.equal(existsSync(resolve(root, "pc/builtin-clawpkgs/hachimi-weather")), false);
});

test("维护导航使用当前目录而非工程变更记录", () => {
  for (const path of [".folder.md", "pc/.folder.md", "pc/src/.folder.md", "pc/scripts/.folder.md"]) {
    const text = read(path);
    assert.ok(text.includes("EasyCoding Deck"), path);
    assert.doesNotMatch(text, /\bP4\b|HachimoDock-main|哈基米机|已移除的|已退休的|原根目录/);
  }
  const instructions = read("CLAUDE.md");
  assert.doesNotMatch(instructions, /\bP4\b|HachimoDock-main|哈基米机|已移除的|已退休的|原根目录/);
});
