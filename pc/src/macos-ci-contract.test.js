/** 仅检查 macOS CI 的目标与产物目录契约，不执行编译、签名或挂载。 */
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { desktopBuildPaths } from "../scripts/desktop-build-artifact.mjs";

const repositoryRoot = fileURLToPath(new URL("../../", import.meta.url));
const read = name => readFileSync(join(repositoryRoot, name), "utf8");

test("macOS CI 显式向 Tauri 传入必需的 Cargo target，避免产物路径分歧", () => {
  const { scripts } = JSON.parse(read("pc/package.json"));
  assert.ok(scripts["build:mac:ci"].includes('--target "${CARGO_BUILD_TARGET:?CARGO_BUILD_TARGET is required}"'));
  assert.ok(scripts["build:mac:ci"].includes("--verbose"));
  assert.ok(scripts["build:mac:ci"].includes("--config src-tauri/tauri.macos-ci.conf.json"));
  assert.ok(!scripts["build:mac:local"].includes("CARGO_BUILD_TARGET is required"));
});

for (const triple of ["aarch64-apple-darwin", "x86_64-apple-darwin"]) {
  test(`macOS CI 的签名与下载产物目录跟随 ${triple}`, () => {
    const paths = desktopBuildPaths({ repositoryRoot, target: "macos", env: { CARGO_BUILD_TARGET: triple } });
    assert.equal(paths.bundleRoot, join(repositoryRoot, "pc/src-tauri/target", triple, "release/bundle"));
    const workflow = read(".github/workflows/macos-build.yml");
    assert.ok(workflow.includes("CARGO_BUILD_TARGET: ${{ matrix.target }}"));
    assert.ok(workflow.includes('bundle="pc/src-tauri/target/$CARGO_BUILD_TARGET/release/bundle"'));
  });
}
