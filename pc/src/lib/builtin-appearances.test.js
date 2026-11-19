/**
 * [Input] Built-in appearance descriptors.
 * [Output] Node test coverage for the twelve protected built-ins, stable ordering/deduplication, packaged media, and Clawd's default WAV cues.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import {
  BUILTIN_TERRIER_APPEARANCE_ID,
  listBuiltinAppearances,
  mergeBuiltinAppearances,
} from "./builtin-appearances.js";

test("listBuiltinAppearances exposes Clawd clip set as a built-in appearance", () => {
  const appearances = listBuiltinAppearances();
  const [terrier] = appearances;

  assert.equal(appearances.length, 12);
  assert.equal(new Set(appearances.map((record) => record.id)).size, 12);
  assert.equal(appearances.every((record) => record.type === "builtin"), true);
  assert.equal(terrier.id, BUILTIN_TERRIER_APPEARANCE_ID);
  assert.equal(terrier.name, "Clawd");
  assert.equal(terrier.type, "builtin");
  assert.equal(terrier.families.length, 17);
  assert.equal(terrier.families[0].family, "welcome");
  assert.match(terrier.families[0].videoSrc, /\/terrier-clips\/welcome\.mp4$/);
  const working = terrier.families.find((family) => family.family === "working");
  assert.match(working.videoSrc, /\/terrier-clips\/working\.thinking\.mp4$/);
  const done = terrier.families.find((family) => family.family === "done");
  const error = terrier.families.find((family) => family.family === "error");
  const waitingUser = terrier.families.find((family) => family.family === "waiting_user");
  assert.match(done.audioSrc, /\/terrier-clips\/done\.wav$/);
  assert.match(done.audioPath, /done\.wav$/);
  assert.equal(done.audioDefault, true);
  assert.match(error.audioSrc, /\/terrier-clips\/error\.wav$/);
  assert.match(error.audioPath, /error\.wav$/);
  assert.equal(error.audioDefault, true);
  assert.match(waitingUser.audioSrc, /\/terrier-clips\/waiting_user\.wav$/);
  assert.match(waitingUser.audioPath, /waiting_user\.wav$/);
  assert.equal(waitingUser.audioDefault, true);
  assert.equal(terrier.families.find((family) => family.family === "working.typing").audioSrc, undefined);
  assert.deepEqual(
    terrier.families
      .map((family) => family.family)
      .filter((family) => family.startsWith("working.")),
    ["working.thinking", "working.typing", "working.browsing"],
  );
  assert.equal(terrier.families.some((family) => family.family === "working"), true);
  assert.equal(terrier.families.some((family) => family.family === "working.default"), false);
});

test("all additional built-ins point at packaged previewable videos", () => {
  const publicDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "public");
  for (const appearance of listBuiltinAppearances().slice(1)) {
    assert.ok(appearance.families.length >= 9, `${appearance.id} has state clips`);
    for (const family of appearance.families) {
      assert.ok(family.videoSrc.startsWith(`/builtin-appearances/${appearance.id}/videos/`));
      assert.equal(existsSync(join(publicDir, family.videoSrc.slice(1))), true, family.videoSrc);
    }
  }
});

test("mergeBuiltinAppearances keeps all built-ins first and avoids duplicate ids", () => {
  const merged = mergeBuiltinAppearances([
    { id: "user-pet", created_at: "2026-05-13T00:00:00.000Z" },
    { id: BUILTIN_TERRIER_APPEARANCE_ID, created_at: "2026-05-14T00:00:00.000Z" },
  ]);

  assert.deepEqual(
    merged.map((record) => record.id),
    [...listBuiltinAppearances().map((record) => record.id), "user-pet"],
  );
});
