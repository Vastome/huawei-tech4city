import assert from "node:assert/strict";
import { test } from "node:test";

import {
  cellsToHardwarePayload,
  cellsToWokwiBatchPayload,
  dotsToPinFrame,
  textToBraille,
} from "../lib/braille.ts";

test("browser and C++ use the same six-dot capital and number cells", () => {
  const cells = textToBraille("A12 b");
  assert.deepEqual(cells.map((cell) => dotsToPinFrame(cell.dots)), [
    "00000100", // capital sign: dot 6
    "10000000", // A: dot 1
    "00111100", // number sign: dots 3-6
    "10000000", // 1: a
    "11000000", // 2: b
    "00000000", // space
    "11000000", // b
  ]);
  assert.equal(cellsToHardwarePayload(cells).startsWith("PINS:00000100,10000000"), true);
  assert.equal(cellsToWokwiBatchPayload(cells, 700, 233).startsWith("BATCH:700,233|00000100"), true);
});

test("number mode ends at punctuation and unsupported text stays in six dots", () => {
  const cells = textToBraille("3.4 @");
  assert.deepEqual(cells.map((cell) => cell.label), [
    "number sign", "digit 3", ".", "number sign", "digit 4", "space", "?",
  ]);
  assert.ok(cells.every((cell) => dotsToPinFrame(cell.dots).endsWith("00")));
});
