/**
 * [Input] 仓库内两个带 Node shebang 的 bridge 测试 CLI；不调用真实 CLI。
 * [Output] 在 POSIX 上恢复测试脚本执行位，Windows 不修改权限。
 * [Pos] test:bridge 的离线准备步骤，兼容从 Windows 重新初始化的 Git 仓库。
 * [Sync] 修改本文件时同步 scripts/.folder.md。
 */
import { chmodSync, lstatSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const defaultRoot = fileURLToPath(new URL("../src-tauri/bridge/packages/agent-session-bus/test/fixtures/", import.meta.url));

export function prepareBridgeTestFixtures({ platform = process.platform, fixtureRoot = defaultRoot } = {}) {
  if (platform === "win32") return;
  const files = ["fake-claude.js", "fake-codex.js"].map(name => resolve(fixtureRoot, name));
  // 先核验完整白名单，再改变权限；不跟随文件链接，不执行脚本。
  const fixtures = files.map(file => {
    const stat = lstatSync(file);
    if (!stat.isFile() || !readFileSync(file, "utf8").startsWith("#!/usr/bin/env node\n")) {
      throw new Error(`测试 CLI 必须是带 Node shebang 的普通文件：${file}`);
    }
    return { file, mode: stat.mode & 0o777 };
  });
  for (const { file, mode } of fixtures) {
    if ((mode & 0o111) !== 0o111) chmodSync(file, mode | 0o111);
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  prepareBridgeTestFixtures();
}
