/** 只使用临时假脚本验证执行位准备；不调用真实 CLI、不联网。 */
import test from "node:test";
import assert from "node:assert/strict";
import { chmodSync, mkdtempSync, readFileSync, rmSync, statSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { prepareBridgeTestFixtures } from "../scripts/prepare-bridge-test-fixtures.mjs";

const posix = { skip: process.platform === "win32" };
const names = ["fake-claude.js", "fake-codex.js"];
const source = "#!/usr/bin/env node\nthrow new Error('不得执行测试脚本');\n";

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), "bridge-cli-permissions-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  for (const name of names) {
    const path = join(root, name);
    writeFileSync(path, source);
    chmodSync(path, 0o644);
  }
  return root;
}

test("Windows 不访问或修改 POSIX 测试文件权限", () => {
  assert.doesNotThrow(() => prepareBridgeTestFixtures({ platform: "win32", fixtureRoot: "不存在的测试目录" }));
});

test("POSIX 恢复两个固定假 CLI 的执行位，且不改内容、不运行脚本", posix, t => {
  const root = fixture(t);
  const unrelated = join(root, "unrelated.js");
  writeFileSync(unrelated, source);
  chmodSync(unrelated, 0o644);
  prepareBridgeTestFixtures({ fixtureRoot: root });
  prepareBridgeTestFixtures({ fixtureRoot: root });
  for (const name of names) {
    assert.equal(statSync(join(root, name)).mode & 0o777, 0o755);
    assert.equal(readFileSync(join(root, name), "utf8"), source);
  }
  assert.equal(statSync(unrelated).mode & 0o777, 0o644);
});

test("非 Node shebang 在任何权限修改前报错", posix, t => {
  const root = fixture(t);
  writeFileSync(join(root, names[1]), "不是可执行测试 CLI\n");
  assert.throws(() => prepareBridgeTestFixtures({ fixtureRoot: root }), /Node shebang/);
  assert.equal(statSync(join(root, names[0])).mode & 0o777, 0o644);
});

test("不跟随指向其他文件的符号链接", posix, t => {
  const root = fixture(t);
  const external = join(root, "external.js");
  writeFileSync(external, source);
  chmodSync(external, 0o644);
  rmSync(join(root, names[1]));
  symlinkSync(external, join(root, names[1]));
  assert.throws(() => prepareBridgeTestFixtures({ fixtureRoot: root }), /普通文件/);
  assert.equal(statSync(external).mode & 0o777, 0o644);
  assert.equal(statSync(join(root, names[0])).mode & 0o777, 0o644);
});
