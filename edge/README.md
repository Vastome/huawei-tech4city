# Vastome C++ Edge Prototype

This module is the computer-testable version of the planned Raspberry Pi + ESP32
product. It does not replace the browser demo. It isolates the production logic
so the same OCR, Braille encoding, packet validation and retry behaviour can be
tested before the physical boards arrive.

## What is simulated

```text
line image -> OpenCV cleanup -> Tesseract OCR -> 6-dot Braille encoder
           -> CRC packet -> simulated serial link -> virtual ESP32 -> six pins
```

- Raspberry Pi side: image preprocessing, OCR, Braille translation, sequencing.
- ESP32 side: packet validation, duplicate protection and six actuator outputs.
- Faults: deterministic packet loss or corruption to verify ACK/retry handling.
- Accuracy: repeatable images with known text and a CSV report using character
  error rate. Generated samples are saved for inspection.

## Build on macOS

```bash
brew install cmake opencv tesseract
cmake -S edge -B edge/build -DCMAKE_BUILD_TYPE=Release
cmake --build edge/build --parallel
ctest --test-dir edge/build --output-on-failure
```

## Run the simulator

With a generated line:

```bash
./edge/build/vastome_edge_sim \
  --synthetic "Hello my name is Long" \
  --expected "Hello my name is Long"
```

With a real cropped camera image:

```bash
./edge/build/vastome_edge_sim \
  --image /absolute/path/to/printed-line.jpg \
  --expected "known ground truth"
```

Prove that retry works while every second transmission is dropped:

```bash
./edge/build/vastome_edge_sim --synthetic "abc" --drop-every 2 --trace
```

`--trace` labels the boundary between the simulated Raspberry Pi, UART link,
ESP32 packet validation and six GPIO/actuator states. These are software models
running on the computer; they are not proof that physical boards were contacted.

## Run the repeatable benchmark

```bash
./edge/build/vastome_accuracy_benchmark edge/out/benchmark
```

The command creates `results.csv` and all input images. The included benchmark
is a regression test, not a real-world accuracy claim. Before the final, add a
separate labelled dataset captured with the intended camera, distance, paper,
font sizes, lighting and hand movement. Do not tune on the same images used for
the final evaluation.

For real camera samples, create a tab-separated manifest (one sample per line):

```text
captures/01.jpg<TAB>Hello my name is Long
captures/02.jpg<TAB>Braille opens books
```

`<TAB>` means one real Tab key, not the literal characters. Image paths are
resolved relative to the manifest file. Then run:

```bash
./edge/build/vastome_dataset_benchmark \
  /absolute/path/to/manifest.tsv \
  /absolute/path/to/real-results.csv
```

This reports mean character accuracy, perfect-line rate, latency, unreadable
samples, and the per-image result needed for a credible competition slide.

## Moving to physical boards

Keep `ImagePipeline`, `BrailleEncoder` and the wire protocol unchanged. Replace:

1. the image-file input with the Raspberry Pi camera capture;
2. `SimulatedSerialLink` with a `FrameTransport` implementation for UART/USB
   serial on the Pi;
3. `VirtualEsp32::cell()` with six protected actuator driver outputs.

Do not connect actuators directly to GPIO. The physical design needs a suitable
driver, flyback protection, an external actuator supply and a shared ground.
