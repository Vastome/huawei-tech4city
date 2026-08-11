import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

const projectRoot = new URL("../", import.meta.url);

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("https://prototype.example/", {
      headers: {
        accept: "text/html",
        host: "prototype.example",
        "x-forwarded-proto": "https",
      },
    }),
    { ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) } },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the Micro-Tactile Reader prototype", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /Micro-Tactile Reader/);
  assert.match(html, /Live OCR to Text/);
  assert.match(html, /From a printed line/);
  assert.match(html, /Start camera/);
  assert.match(html, /Send state/i);
  assert.match(html, /Hardware \+ software demo/);
  assert.match(html, /Companion Pico demo included/);
  assert.match(html, /Send OCR text directly/);
  assert.match(html, /Wokwi simulator/);
  assert.match(html, /Real Pico USB/);
  assert.match(html, /Send to focused Wokwi/);
  assert.match(html, /Copy Wokwi command/);
  assert.match(html, /Cell hold \(ms\)/);
  assert.match(html, /https:\/\/prototype\.example\/og\.png/);
  assert.doesNotMatch(html, /codex-preview|Your site is taking shape/);
});

test("keeps the MVP scope honest and includes required assets", async () => {
  const [page, layout, packageJson] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/layout.tsx", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(page, /navigator\.mediaDevices\.getUserMedia/);
  assert.match(page, /navigator\.serial\?\.requestPort/);
  assert.match(page, /readable: ReadableStream<Uint8Array> \| null/);
  assert.match(page, /await import\("tesseract\.js"\)/);
  assert.match(page, /PSM\.SINGLE_LINE/);
  assert.match(page, /getGuideCrop/);
  assert.match(page, /adaptive-threshold/);
  assert.match(page, /shadow-balanced/);
  assert.match(page, /OCR_VARIANTS = 2/);
  assert.match(page, /PINS:/);
  assert.match(page, /BATCH:/);
  assert.match(page, /CONFIG:/);
  assert.match(page, /ACK CONFIG/);
  assert.match(page, /WOKWI_BRIDGE_ENDPOINT/);
  assert.match(page, /115200 baud/);
  assert.match(page, /python run_project\.py bridge/);
  assert.match(page, /Match current on-screen speed/);
  assert.match(page, /Handwriting, complex layouts/);
  assert.match(page, /real-Pico streaming or Wokwi simulator batch handoff/);
  assert.match(page, /Send state/);
  assert.match(layout, /generateMetadata/);
  assert.match(packageJson, /"tesseract\.js"/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);

  await access(new URL("../public/og.png", import.meta.url));
  await access(new URL("../public/favicon.svg", import.meta.url));
  await assert.rejects(access(new URL("../app/_sites-preview", projectRoot)));
});
