import test from "node:test";
import assert from "node:assert/strict";

import { assignButtonAction } from "./button-action-mapping.js";

const rows = [1, 2, 3, 4, 5].map((key) => ({
  id: `s3_sw${key}_short`,
  event: `button.sw${key}.short_press`,
}));

test("moving S3 confirm from key 1 to key 2 makes key 2 the only confirm key", () => {
  const actions = {
    s3_sw1_short: "page_enter",
    s3_sw2_short: "session_previous",
    s3_sw3_short: "session_next",
    s3_sw4_short: "page_back",
    s3_sw5_short: "component_center",
  };
  const next = assignButtonAction(rows, actions, rows[1], "page_enter");
  assert.equal(next.s3_sw1_short, "disabled");
  assert.equal(next.s3_sw2_short, "page_enter");
  assert.deepEqual(
    Object.entries(next).filter(([, action]) => action === "page_enter").map(([id]) => id),
    ["s3_sw2_short"],
  );
});

test("moving S3 cancel preserves confirm and makes cancel unique", () => {
  const actions = {
    s3_sw1_short: "page_enter",
    s3_sw2_short: "session_previous",
    s3_sw3_short: "session_next",
    s3_sw4_short: "page_back",
    s3_sw5_short: "component_center",
  };
  const next = assignButtonAction(rows, actions, rows[2], "page_back");
  assert.equal(next.s3_sw1_short, "page_enter");
  assert.equal(next.s3_sw3_short, "page_back");
  assert.equal(next.s3_sw4_short, "disabled");
});

test("ordinary actions remain repeatable", () => {
  const actions = { s3_sw1_short: "session_next", s3_sw2_short: "disabled" };
  const next = assignButtonAction(rows, actions, rows[1], "session_next");
  assert.equal(next.s3_sw1_short, "session_next");
  assert.equal(next.s3_sw2_short, "session_next");
});
