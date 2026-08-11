# Huawei Tech4City Software — Live OCR to Text

Working software prototype for the Tech4City 2026 Semi-Final.

Canonical repository:
https://github.com/Vastome/huawei-tech4city

This folder is the software half of the combined workspace. The companion Pico and Wokwi hardware prototype now lives in the sibling `hardware/` folder.

Note: this folder is maintained inside the main repository above (it is not a separate Git repository).

The prototype captures one line of clear printed English through a live camera,
runs OCR locally in the browser, and sends OCR-derived pin frames to the
selected hardware target (Wokwi bridge or real Pico via Web Serial).

## What is working

- Live browser camera; no image upload.
- A central guide for capturing one printed line.
- Local printed-text OCR with confidence feedback.
- Shadow-balanced and adaptive-threshold image enhancement.
- Preview-aligned cropping on desktop and mobile cameras.
- Two local OCR passes with automatic best-result selection.
- Grade 1 conversion logic that produces dot1-dot8 pin frames for hardware.
- Direct Web Serial handoff to the Pico using `PINS:` dot1-dot8 bit frames.
- Browser-side timing control using `CONFIG:` before each hardware send.
- One-line Wokwi simulator handoff using `BATCH:<hold>,<blink>|<frames...>`.
- OCR send-state reporting: waiting, sending, sent, failed.
- Honest error states and a clearly stated validation scope.

## Current scope

Supported: clear printed English, one line at a time, under adequate lighting.

Not yet supported: handwriting, complex page layouts, translation,
summarisation, fingertip tracking or a physical Braille actuator.

## Run locally

Requirements: Node.js 22.13 or newer.

```bash
cd software
npm install
npm run dev
```

Open `http://localhost:3000`, select **Start camera**, permit camera access,
align a single printed line inside the guide, and select **Scan printed line**.

Camera access requires either `localhost` or a secure HTTPS deployment.

For direct hardware handoff, use current Chrome or Edge on desktop, select
**Connect Pico**, and allow Web Serial access to the board.

The app now sends two serial commands:

- `CONFIG:<holdMs>,<blinkMs>` to sync playback timing on the Pico
- `PINS:<frame>,<frame>,...` where each frame is 8 bits in dot order `1..8`

The UI waits for Pico replies such as `ACK CONFIG ...` and
`OK remote frames displayed` before reporting a successful handoff.

For the Wokwi simulator, start the local bridge:

```bash
python run_project.py bridge
```

Then switch the handoff target to **Wokwi simulator** and keep the Wokwi Serial
Monitor focused. The app auto-sends the generated `BATCH:` command to the local
bridge after each successful scan. **Copy Wokwi command** remains available as
a manual fallback.

## Verify

```bash
npm test
```

This builds the production bundle and verifies the rendered prototype,
metadata, scope statements, OCR integration and project assets.

## Suggested 60-second demonstration

1. Start the camera and show that there is no uploaded or preloaded image.
2. Hold up a card that reads `BRAILLE OPENS BOOKS`.
3. Align it in the guide and scan the line.
4. Show the recognized text and confidence score.
5. Show send state moving to `Sending` then `Sent` as the same line is pushed into the Pico/Wokwi demo in `hardware/`.
6. Replace the card with `READ INDEPENDENTLY` and scan again to prove the input is not hard-coded.

## Judge handoff

Share the HTTPS prototype URL together with:

- Browser recommendation: current Chrome, Edge or Safari.
- For direct hardware handoff, prefer Chrome or Edge because Safari does not expose Web Serial.
- Permission instruction: allow camera access when prompted.
- Scope: printed English only; one line at a time.
- A 60–90 second fallback video.
- A note that OCR text handoff is live and local, with no uploaded frames.

All camera frames and OCR processing remain local to the visitor's browser.
