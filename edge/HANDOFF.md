# C++ Edge Prototype Handoff

Branch: `feature/cpp-edge-simulator`

This branch adds a computer-testable C++ version of the proposed Raspberry Pi
and ESP32 software path. The browser demo and the original Pico/Wokwi demo are
still present and unchanged in purpose.

## Current architecture

```text
line image
  -> OpenCV preprocessing
  -> Tesseract single-line OCR
  -> uncontracted 6-dot Braille encoding
  -> CRC-protected frame with sequence number
  -> simulated UART with fault injection
  -> simulated ESP32 ACK and six actuator outputs
```

The Raspberry Pi and ESP32 are currently software models running on the Mac.
No command in this branch contacts a physical board yet.

## Implemented

- CMake-based C++20 library and command-line applications.
- Image normalization: grayscale conversion, resize, deskew, shadow balancing,
  CLAHE, Otsu thresholding and adaptive thresholding.
- Tesseract OCR with the best result selected from three preprocessing paths.
- Image quality metrics and guidance for darkness, glare, blur, skew and line
  position.
- Uncontracted English 6-dot Braille for letters, capitals, numbers, spaces and
  common punctuation.
- Wire format: `BRL|sequence|bits|hold_ms|CRC16`.
- ESP32-side frame validation, duplicate protection and ACK/ERR replies.
- Retry handling for dropped or corrupted packets.
- `--trace` output showing the simulated Pi, UART, ESP32 and GPIO boundaries.
- Synthetic degradation benchmark covering rotation, shadows, blur, low
  contrast, noise and combined degradation.
- Manifest-based evaluator for labelled real camera images.
- Unit tests for Braille indicators, protocol round trips, CRC rejection,
  retry behaviour, character error rate and clear-line OCR.
- Root `run_project.py edge` build/test command, and C++ checks included in
  `run_project.py verify`.

## Verified on the handoff machine

```text
C++ unit tests:                 PASS
Synthetic OCR cases:           9/9 exact
Synthetic mean char accuracy:  100% (regression only)
Synthetic mean latency:        about 80 ms on Apple Silicon Mac
Packet-loss simulation:        retries without a lost cell
Existing web build/tests/lint:  PASS
Existing Pico firmware build:  PASS
```

The synthetic result is not a real-world accuracy claim. A labelled dataset
captured using the intended camera is still required.

## Build and run

macOS dependencies:

```bash
brew install cmake opencv tesseract
```

From the repository root:

```bash
python3 run_project.py edge

./edge/build/vastome_edge_sim \
  --synthetic "Hello" \
  --expected "Hello" \
  --drop-every 4 \
  --trace

./edge/build/vastome_accuracy_benchmark edge/out/benchmark
```

Full repository verification:

```bash
python3 run_project.py verify
```

See `edge/README.md` for the real-image manifest format.

## Not implemented yet

The current simulator recognizes one complete image and then streams every
Braille cell using a fixed hold time. It does **not** yet synchronize the cell
with the user's finger position.

Missing production work:

- Continuous Raspberry Pi camera capture.
- OCR output with a bounding box for every character.
- Optical-flow or text-box tracking between OCR frames.
- A fixed reading cursor calibrated to the fingertip contact point.
- Debounce and hysteresis so a character is not emitted repeatedly.
- Direction tracking for left-to-right reading and reverse rereading.
- Speed detection and a slow-down warning when one physical cell cannot keep
  up with the finger.
- A real UART/USB implementation of `FrameTransport`.
- ESP32 firmware implementing the current frame parser and protected actuator
  driver outputs.
- Real actuator timing, force, temperature and power tests.
- Labelled real-camera accuracy and latency evaluation.

## Recommended next implementation order

1. **Position-controlled desktop simulator**
   - Render recognized character boxes and a fixed reading cursor.
   - Drive the cursor with a slider or recorded movement trace.
   - Emit a new Braille cell only when a new box crosses the cursor.
   - Test pauses, reverse movement, jitter and skipped boxes.

2. **Continuous camera pipeline on the computer**
   - Run tracking at camera frame rate.
   - Run OCR asynchronously and less frequently.
   - Keep several recognized characters ahead of the cursor in a ring buffer,
     so OCR is not on the immediate tactile-output path.

3. **Raspberry Pi transport**
   - Implement `FrameTransport` over a configurable serial device.
   - Add reconnect, timeout and metrics for capture-to-ACK latency.

4. **ESP32 firmware**
   - Port the parser/CRC test vectors from `protocol.cpp`.
   - Map six output bits to a protected MOSFET/driver stage.
   - Add a watchdog and an all-dots-off fail-safe state.

5. **Physical validation**
   - Start with LEDs instead of actuators.
   - Verify packet-to-pin mapping and failure recovery.
   - Then test one actuator, followed by the complete six-dot cell.
   - Measure mechanical response before setting the allowed finger speed.

## Important hardware boundary

Never drive a coil or solenoid directly from a Raspberry Pi or ESP32 GPIO pin.
Use a correctly rated driver, flyback protection, an external actuator supply
and a shared ground. Hardware limits must be confirmed from the selected cell,
not inferred from this simulator.

