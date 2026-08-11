"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

type SerialPortHandle = {
  open: (options: { baudRate: number }) => Promise<void>;
  close: () => Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
};

type PendingHardwareReply = {
  expectedPrefix: string;
  reject: (error: Error) => void;
  resolve: (line: string) => void;
  timeoutId: number;
};

type OcrSendState = "waiting" | "sending" | "sent" | "failed";

type HandoffTarget = "wokwi" | "pico";

const WOKWI_BRIDGE_ENDPOINT =
  process.env.NEXT_PUBLIC_WOKWI_BRIDGE_ENDPOINT ?? "http://127.0.0.1:8765/paste";

declare global {
  interface Navigator {
    serial?: {
      requestPort: () => Promise<SerialPortHandle>;
    };
  }
}

type BrailleCell = {
  dots: number[];
  label: string;
  source: string;
  unicode: string;
};

type OcrWorker = {
  recognize: (image: HTMLCanvasElement) => Promise<{
    data: { confidence: number; text: string };
  }>;
  setParameters: (parameters: Record<string, string>) => Promise<unknown>;
  terminate: () => Promise<unknown>;
};

type OcrCandidate = {
  canvas: HTMLCanvasElement;
  confidence: number;
  label: string;
  score: number;
  text: string;
};

type ImageVariant = {
  canvas: HTMLCanvasElement;
  label: string;
};

const GUIDE = { x: 0.04, y: 0.36, width: 0.92, height: 0.28 } as const;
const OCR_VARIANTS = 2;

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}

function createCanvas(width: number, height: number) {
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(width));
  canvas.height = Math.max(1, Math.round(height));
  return canvas;
}

function getGuideCrop(video: HTMLVideoElement) {
  const sourceWidth = video.videoWidth;
  const sourceHeight = video.videoHeight;
  const displayWidth = video.clientWidth || sourceWidth;
  const displayHeight = video.clientHeight || sourceHeight;

  // The preview uses object-fit: cover. Translate the visible guide back to
  // source-video coordinates so phones and desktop cameras capture exactly
  // what appears between the guide lines.
  const coverScale = Math.max(displayWidth / sourceWidth, displayHeight / sourceHeight);
  const overflowX = (sourceWidth * coverScale - displayWidth) / 2;
  const overflowY = (sourceHeight * coverScale - displayHeight) / 2;
  const x = (displayWidth * GUIDE.x + overflowX) / coverScale;
  const y = (displayHeight * GUIDE.y + overflowY) / coverScale;
  const width = (displayWidth * GUIDE.width) / coverScale;
  const height = (displayHeight * GUIDE.height) / coverScale;

  return {
    x: clamp(x, 0, sourceWidth - 1),
    y: clamp(y, 0, sourceHeight - 1),
    width: clamp(width, 1, sourceWidth - x),
    height: clamp(height, 1, sourceHeight - y),
  };
}

function grayscale(image: ImageData) {
  const values = new Uint8ClampedArray(image.width * image.height);
  for (let pixel = 0, index = 0; pixel < values.length; pixel += 1, index += 4) {
    values[pixel] = Math.round(
      image.data[index] * 0.299 +
      image.data[index + 1] * 0.587 +
      image.data[index + 2] * 0.114,
    );
  }
  return values;
}

function makeIntegralImage(values: Uint8ClampedArray, width: number, height: number) {
  const stride = width + 1;
  const integral = new Float64Array(stride * (height + 1));
  for (let y = 1; y <= height; y += 1) {
    let rowSum = 0;
    for (let x = 1; x <= width; x += 1) {
      rowSum += values[(y - 1) * width + x - 1];
      integral[y * stride + x] = integral[(y - 1) * stride + x] + rowSum;
    }
  }
  return integral;
}

function localMean(
  integral: Float64Array,
  width: number,
  height: number,
  x: number,
  y: number,
  radius: number,
) {
  const stride = width + 1;
  const left = Math.max(0, x - radius);
  const right = Math.min(width - 1, x + radius);
  const top = Math.max(0, y - radius);
  const bottom = Math.min(height - 1, y + radius);
  const sum =
    integral[(bottom + 1) * stride + right + 1] -
    integral[top * stride + right + 1] -
    integral[(bottom + 1) * stride + left] +
    integral[top * stride + left];
  return sum / ((right - left + 1) * (bottom - top + 1));
}

function imageDataFromGrey(values: Uint8ClampedArray, width: number, height: number) {
  const image = new ImageData(width, height);
  for (let pixel = 0, index = 0; pixel < values.length; pixel += 1, index += 4) {
    image.data[index] = values[pixel];
    image.data[index + 1] = values[pixel];
    image.data[index + 2] = values[pixel];
    image.data[index + 3] = 255;
  }
  return image;
}

function findTextBounds(binary: Uint8ClampedArray, width: number, height: number) {
  const rowInk = new Uint32Array(height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      if (binary[y * width + x] === 0) rowInk[y] += 1;
    }
  }

  const minimumRowInk = Math.max(4, Math.round(width * 0.004));
  let top = 0;
  let bottom = height - 1;
  while (top < bottom && rowInk[top] < minimumRowInk) top += 1;
  while (bottom > top && rowInk[bottom] < minimumRowInk) bottom -= 1;

  const contentHeight = Math.max(1, bottom - top + 1);
  const columnInk = new Uint32Array(width);
  for (let x = 0; x < width; x += 1) {
    for (let y = top; y <= bottom; y += 1) {
      if (binary[y * width + x] === 0) columnInk[x] += 1;
    }
  }

  // Ignore almost-solid vertical edges, which are usually page edges or
  // hard shadows rather than characters.
  const minimumColumnInk = Math.max(2, Math.round(contentHeight * 0.025));
  const maximumColumnInk = Math.max(minimumColumnInk + 1, Math.round(contentHeight * 0.82));
  let left = 0;
  let right = width - 1;
  while (
    left < right &&
    (columnInk[left] < minimumColumnInk || columnInk[left] > maximumColumnInk)
  ) left += 1;
  while (
    right > left &&
    (columnInk[right] < minimumColumnInk || columnInk[right] > maximumColumnInk)
  ) right -= 1;

  if (right - left < width * 0.08 || bottom - top < height * 0.08) {
    return { left: 0, top: 0, right: width - 1, bottom: height - 1 };
  }

  const xPadding = Math.max(18, Math.round((right - left + 1) * 0.035));
  const yPadding = Math.max(12, Math.round((bottom - top + 1) * 0.22));
  return {
    left: Math.max(0, left - xPadding),
    top: Math.max(0, top - yPadding),
    right: Math.min(width - 1, right + xPadding),
    bottom: Math.min(height - 1, bottom + yPadding),
  };
}

function renderVariant(image: ImageData, bounds: ReturnType<typeof findTextBounds>) {
  const source = createCanvas(image.width, image.height);
  const sourceContext = source.getContext("2d");
  if (!sourceContext) throw new Error("Unable to prepare the enhanced image.");
  sourceContext.putImageData(image, 0, 0);

  const cropWidth = bounds.right - bounds.left + 1;
  const cropHeight = bounds.bottom - bounds.top + 1;
  const scale = clamp(220 / cropHeight, 1, 2.5);
  const outputWidth = Math.min(1800, Math.max(700, Math.round(cropWidth * scale)));
  const outputHeight = Math.max(150, Math.round(cropHeight * (outputWidth / cropWidth)));
  const output = createCanvas(outputWidth, outputHeight);
  const outputContext = output.getContext("2d");
  if (!outputContext) throw new Error("Unable to render the enhanced image.");
  outputContext.fillStyle = "#fff";
  outputContext.fillRect(0, 0, output.width, output.height);
  outputContext.imageSmoothingEnabled = true;
  outputContext.imageSmoothingQuality = "high";
  outputContext.drawImage(
    source,
    bounds.left,
    bounds.top,
    cropWidth,
    cropHeight,
    0,
    0,
    output.width,
    output.height,
  );
  return output;
}

function buildImageVariants(source: HTMLCanvasElement): ImageVariant[] {
  const context = source.getContext("2d", { willReadFrequently: true });
  if (!context) throw new Error("Unable to read the captured frame.");
  const image = context.getImageData(0, 0, source.width, source.height);
  const grey = grayscale(image);
  const integral = makeIntegralImage(grey, image.width, image.height);
  const radius = clamp(Math.round(image.height * 0.1), 12, 42);
  const normalized = new Uint8ClampedArray(grey.length);
  const binary = new Uint8ClampedArray(grey.length);
  let inkPixels = 0;

  for (let y = 0; y < image.height; y += 1) {
    for (let x = 0; x < image.width; x += 1) {
      const index = y * image.width + x;
      const mean = localMean(integral, image.width, image.height, x, y, radius);
      const difference = grey[index] - mean;
      normalized[index] = clamp(Math.round(232 + difference * 2.25), 0, 255);
      const threshold = mean - Math.max(9, mean * 0.12);
      const isInk = grey[index] < threshold;
      binary[index] = isInk ? 0 : 255;
      if (isInk) inkPixels += 1;
    }
  }

  const inkRatio = inkPixels / binary.length;
  if (inkRatio < 0.0015) {
    throw new Error("No clear text was found inside the guide. Move the line closer and scan again.");
  }

  const bounds = findTextBounds(binary, image.width, image.height);
  return [
    {
      label: "shadow-balanced",
      canvas: renderVariant(imageDataFromGrey(normalized, image.width, image.height), bounds),
    },
    {
      label: "adaptive-threshold",
      canvas: renderVariant(imageDataFromGrey(binary, image.width, image.height), bounds),
    },
  ];
}

function cleanOcrText(text: string) {
  return text.replace(/\s+/g, " ").trim();
}

function scoreOcrResult(text: string, confidence: number) {
  const visibleCharacters = text.replace(/\s/g, "");
  const words = text.split(/\s+/).filter(Boolean);
  const readableRatio = visibleCharacters.length
    ? (visibleCharacters.match(/[A-Za-z0-9]/g)?.length ?? 0) / visibleCharacters.length
    : 0;
  return (
    confidence +
    Math.min(18, visibleCharacters.length * 0.72) +
    Math.min(5, words.length) -
    (1 - readableRatio) * 18
  );
}

const LETTER_DOTS: Record<string, number[]> = {
  a: [1], b: [1, 2], c: [1, 4], d: [1, 4, 5], e: [1, 5],
  f: [1, 2, 4], g: [1, 2, 4, 5], h: [1, 2, 5], i: [2, 4], j: [2, 4, 5],
  k: [1, 3], l: [1, 2, 3], m: [1, 3, 4], n: [1, 3, 4, 5], o: [1, 3, 5],
  p: [1, 2, 3, 4], q: [1, 2, 3, 4, 5], r: [1, 2, 3, 5], s: [2, 3, 4],
  t: [2, 3, 4, 5], u: [1, 3, 6], v: [1, 2, 3, 6], w: [2, 4, 5, 6],
  x: [1, 3, 4, 6], y: [1, 3, 4, 5, 6], z: [1, 3, 5, 6],
};

const PUNCTUATION_DOTS: Record<string, number[]> = {
  ",": [2], ";": [2, 3], ":": [2, 5], ".": [2, 5, 6],
  "!": [2, 3, 5], "?": [2, 3, 6], "'": [3], "-": [3, 6],
  "(": [1, 2, 6], ")": [3, 4, 5],
};

const SYMBOL_DOTS: Record<string, number[]> = {
  "\"": [5, 6],
  "/": [3, 4],
  "\\": [1, 2, 5, 6],
  "@": [4, 5],
  "#": [3, 4, 5, 6],
  "$": [1, 2, 4, 6],
  "%": [1, 4, 6],
  "&": [1, 2, 3, 4, 6],
  "*": [1, 6],
  "+": [2, 3, 5],
  "=": [1, 2, 3, 4, 5, 6],
  "<": [1, 2, 6],
  ">": [3, 4, 5],
  "[": [2, 4, 6],
  "]": [1, 2, 4, 5, 6],
  "{": [2, 4, 6],
  "}": [1, 2, 4, 5, 6],
  "_": [4, 5, 6],
  "`": [4],
  "^": [4, 5],
  "~": [4, 5, 6],
  "|": [1, 2, 5, 6],
};

const DIGIT_LETTERS: Record<string, string> = {
  "1": "a", "2": "b", "3": "c", "4": "d", "5": "e",
  "6": "f", "7": "g", "8": "h", "9": "i", "0": "j",
};

function dotsToUnicode(dots: number[]) {
  const value = dots.reduce((sum, dot) => sum + (1 << (dot - 1)), 0);
  return String.fromCodePoint(0x2800 + value);
}

function makeCell(dots: number[], label: string, source: string): BrailleCell {
  return { dots, label, source, unicode: dotsToUnicode(dots) };
}

function dotsToPinFrame(dots: number[]) {
  return Array.from({ length: 8 }, (_, index) => (
    dots.includes(index + 1) ? "1" : "0"
  )).join("");
}

function cellsToHardwarePayload(cells: BrailleCell[]) {
  return `PINS:${cells.map((cell) => dotsToPinFrame(cell.dots)).join(",")}\n`;
}

function cellsToWokwiBatchPayload(
  cells: BrailleCell[],
  holdMs: number,
  blinkMs: number,
) {
  return `BATCH:${holdMs},${blinkMs}|${cells.map((cell) => dotsToPinFrame(cell.dots)).join(",")}`;
}

function asciiToDots(character: string) {
  if (character.length !== 1) return null;
  const codePoint = character.codePointAt(0);
  if (codePoint === undefined || codePoint < 32 || codePoint > 126) return null;

  const dots: number[] = [];
  for (let bit = 0; bit < 8; bit += 1) {
    if ((codePoint & (1 << bit)) !== 0) {
      dots.push(bit + 1);
    }
  }

  return dots;
}

function textToBraille(text: string): BrailleCell[] {
  const cells: BrailleCell[] = [];

  for (const character of text) {
    if (/\d/.test(character)) {
      const letter = DIGIT_LETTERS[character];
      cells.push(makeCell(LETTER_DOTS[letter], `digit ${character}`, character));
      continue;
    }

    if (character === " ") {
      cells.push(makeCell([], "space", "space"));
      continue;
    }

    const lower = character.toLowerCase();
    if (LETTER_DOTS[lower]) {
      cells.push(makeCell(LETTER_DOTS[lower], lower, character));
      continue;
    }

    if (PUNCTUATION_DOTS[character]) {
      cells.push(makeCell(PUNCTUATION_DOTS[character], character, character));
      continue;
    }

    if (SYMBOL_DOTS[character]) {
      cells.push(makeCell(SYMBOL_DOTS[character], character, character));
      continue;
    }

    const asciiDots = asciiToDots(character);
    if (asciiDots) {
      cells.push(makeCell(asciiDots, `ascii ${character}`, character));
      continue;
    }

    // Keep transmission stable even for unsupported Unicode by emitting '?'.
    cells.push(makeCell(PUNCTUATION_DOTS["?"], "?", character));
  }

  return cells;
}

const SPEEDS: Record<"Slow" | "Medium" | "Fast", number> = {
  Slow: 1100,
  Medium: 700,
  Fast: 420,
};

const DEFAULT_HARDWARE_CELL_DURATION_MS = 700;

type SpeedName = keyof typeof SPEEDS;

export default function Home() {
  const videoRef = useRef<HTMLVideoElement>(null);
  const captureCanvasRef = useRef<HTMLCanvasElement>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const workerRef = useRef<OcrWorker | null>(null);
  const ocrPassRef = useRef({ index: 0, total: 1 });
  const serialPortRef = useRef<SerialPortHandle | null>(null);
  const serialReaderRef = useRef<ReadableStreamDefaultReader<Uint8Array> | null>(null);
  const hardwareLineBufferRef = useRef("");
  const pendingHardwareReplyRef = useRef<PendingHardwareReply | null>(null);

  const [cameraActive, setCameraActive] = useState(false);
  const [cameraError, setCameraError] = useState("");
  const [isScanning, setIsScanning] = useState(false);
  const [ocrStatus, setOcrStatus] = useState("Ready to scan printed text.");
  const [progress, setProgress] = useState(0);
  const [recognizedText, setRecognizedText] = useState("");
  const [confidence, setConfidence] = useState<number | null>(null);
  const [cells, setCells] = useState<BrailleCell[]>([]);
  const [speed] = useState<SpeedName>("Medium");
  const [hardwareConnected, setHardwareConnected] = useState(false);
  const [isSendingToHardware, setIsSendingToHardware] = useState(false);
  const [handoffTarget, setHandoffTarget] = useState<HandoffTarget>("wokwi");
  const [autoSendToHardware, setAutoSendToHardware] = useState(true);
  const [syncHardwareTiming, setSyncHardwareTiming] = useState(true);
  const [manualHardwareCellDurationMs, setManualHardwareCellDurationMs] = useState(DEFAULT_HARDWARE_CELL_DURATION_MS);
  const [manualHardwareBlinkMs, setManualHardwareBlinkMs] = useState(250);
  const [hardwareStatus, setHardwareStatus] = useState(
    "Wokwi mode is ready. Scan a line and it will auto-send to the focused Wokwi Serial Monitor.",
  );
  const [hardwareError, setHardwareError] = useState("");
  const [lastHardwareReply, setLastHardwareReply] = useState("Waiting for Pico.");
  const [sendState, setSendState] = useState<OcrSendState>("waiting");
  const [sentTextPreview, setSentTextPreview] = useState("");

  const hardwareCellDurationMs = syncHardwareTiming
    ? SPEEDS[speed]
    : manualHardwareCellDurationMs;
  const hardwareBlinkMs = syncHardwareTiming
    ? Math.max(140, Math.round(SPEEDS[speed] / 3))
    : manualHardwareBlinkMs;

  const stopCamera = useCallback(() => {
    streamRef.current?.getTracks().forEach((track) => track.stop());
    streamRef.current = null;
    if (videoRef.current) videoRef.current.srcObject = null;
    setCameraActive(false);
  }, []);

  useEffect(() => {
    return () => {
      streamRef.current?.getTracks().forEach((track) => track.stop());
      void workerRef.current?.terminate();
      const pendingReply = pendingHardwareReplyRef.current;
      if (pendingReply) {
        window.clearTimeout(pendingReply.timeoutId);
        pendingReply.reject(new Error("Hardware connection closed."));
        pendingHardwareReplyRef.current = null;
      }
      if (serialReaderRef.current) {
        void serialReaderRef.current.cancel().catch(() => undefined);
      }
      if (serialPortRef.current) {
        void serialPortRef.current.close().catch(() => undefined);
      }
    };
  }, []);

  const startCamera = async () => {
    setCameraError("");
    setOcrStatus("Requesting camera access…");
    try {
      stopCamera();
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: {
          facingMode: { ideal: "environment" },
          height: { ideal: 1080 },
          width: { ideal: 1920 },
        },
      });
      streamRef.current = stream;
      if (videoRef.current) {
        videoRef.current.srcObject = stream;
        await videoRef.current.play();
      }
      setCameraActive(true);
      setOcrStatus("Camera ready. Align one printed line inside the guide.");
    } catch (error) {
      const message = error instanceof Error ? error.message : "Camera unavailable.";
      setCameraError(`Camera access failed: ${message}`);
      setOcrStatus("Camera permission is required for live OCR.");
    }
  };

  const getWorker = async () => {
    if (workerRef.current) return workerRef.current;

    const { createWorker, OEM, PSM } = await import("tesseract.js");
    const worker = (await createWorker("eng", OEM.LSTM_ONLY, {
      logger: (message) => {
        const pass = ocrPassRef.current;
        const value = Math.round(
          ((pass.index + (message.progress ?? 0)) / pass.total) * 100,
        );
        setProgress(value);
        setOcrStatus(
          message.status === "recognizing text"
            ? `Comparing enhanced image ${pass.index + 1}/${pass.total}… ${value}%`
            : "Preparing local OCR engine…",
        );
      },
    })) as OcrWorker;

    await worker.setParameters({
      preserve_interword_spaces: "1",
      tessedit_char_whitelist:
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,?!'-:;() ",
      tessedit_pageseg_mode: PSM.SINGLE_LINE,
    });
    workerRef.current = worker;
    return worker;
  };

  const handleHardwareLine = useCallback((line: string) => {
    const trimmed = line.trim();
    if (!trimmed) {
      return;
    }

    setLastHardwareReply(trimmed);

    const pendingReply = pendingHardwareReplyRef.current;
    if (pendingReply && trimmed.startsWith("ERR")) {
      window.clearTimeout(pendingReply.timeoutId);
      pendingHardwareReplyRef.current = null;
      pendingReply.reject(new Error(trimmed));
      return;
    }

    if (pendingReply && trimmed.startsWith(pendingReply.expectedPrefix)) {
      window.clearTimeout(pendingReply.timeoutId);
      pendingHardwareReplyRef.current = null;
      pendingReply.resolve(trimmed);
      return;
    }

    if (trimmed.startsWith("FRAME ")) {
      setHardwareStatus(`Pico playback progress: ${trimmed}.`);
      return;
    }

    if (trimmed.startsWith("ACK CONFIG")) {
      setHardwareStatus(`Pico timing acknowledged: ${trimmed}.`);
      return;
    }

    if (trimmed.startsWith("OK remote frames displayed")) {
      setHardwareStatus("Pico confirmed that all direct pin frames were displayed.");
      return;
    }

    if (trimmed.startsWith("Braille LED Display ready")) {
      setHardwareStatus("Pico connected and reporting ready state.");
    }
  }, []);

  const startHardwareReader = useCallback((port: SerialPortHandle) => {
    if (!port.readable) {
      setHardwareStatus("Pico connected, but browser-side acknowledgements are unavailable on this port.");
      return;
    }

    const reader = port.readable.getReader();
    const decoder = new TextDecoder();
    serialReaderRef.current = reader;
    hardwareLineBufferRef.current = "";

    void (async () => {
      try {
        while (true) {
          const { done, value } = await reader.read();
          if (done) {
            break;
          }

          hardwareLineBufferRef.current += decoder.decode(value, { stream: true });
          const segments = hardwareLineBufferRef.current.split(/\r?\n/);
          hardwareLineBufferRef.current = segments.pop() ?? "";
          for (const segment of segments) {
            handleHardwareLine(segment);
          }
        }
      } catch {
        // Reader cancellation is expected on disconnect.
      } finally {
        serialReaderRef.current = null;
        reader.releaseLock();
      }
    })();
  }, [handleHardwareLine]);

  const sendHardwareCommand = async (
    port: SerialPortHandle,
    command: string,
    expectedPrefix: string,
    timeoutMs: number,
  ) => {
    const writer = port.writable?.getWriter();
    if (!writer) {
      throw new Error("The Pico serial port is not writable.");
    }

    if (pendingHardwareReplyRef.current) {
      throw new Error("The Pico is still processing the previous command.");
    }

    try {
      const reply = await new Promise<string>(async (resolve, reject) => {
        const timeoutId = window.setTimeout(() => {
          pendingHardwareReplyRef.current = null;
          reject(new Error(`Timed out waiting for ${expectedPrefix} from the Pico.`));
        }, timeoutMs);

        pendingHardwareReplyRef.current = {
          expectedPrefix,
          reject,
          resolve,
          timeoutId,
        };

        try {
          await writer.write(new TextEncoder().encode(command));
        } catch (error) {
          window.clearTimeout(timeoutId);
          pendingHardwareReplyRef.current = null;
          reject(error instanceof Error ? error : new Error("Serial write failed."));
        }
      });

      return reply;
    } finally {
      writer.releaseLock();
    }
  };

  const connectHardware = async () => {
    setHardwareError("");

    if (!navigator.serial?.requestPort) {
      setHardwareError("This browser does not support Web Serial. Use current Chrome or Edge on desktop.");
      setHardwareStatus("Hardware handoff unavailable in this browser.");
      return;
    }

    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      serialPortRef.current = port;
      startHardwareReader(port);
      setHardwareConnected(true);
      setHandoffTarget("pico");
      setLastHardwareReply("Waiting for Pico.");
      setHardwareStatus("Pico connected and ready for OCR text handoff.");
    } catch (error) {
      const message = error instanceof Error ? error.message : "Serial connection failed.";
      setHardwareError(`Hardware connection failed: ${message}`);
      setHardwareStatus("Unable to open the Pico serial port.");
    }
  };

  const disconnectHardware = async () => {
    setHardwareError("");
    const pendingReply = pendingHardwareReplyRef.current;
    if (pendingReply) {
      window.clearTimeout(pendingReply.timeoutId);
      pendingHardwareReplyRef.current = null;
      pendingReply.reject(new Error("Hardware disconnected before the Pico replied."));
    }
    try {
      await serialReaderRef.current?.cancel();
      await serialPortRef.current?.close();
    } catch (error) {
      const message = error instanceof Error ? error.message : "Serial disconnect failed.";
      setHardwareError(`Hardware disconnect failed: ${message}`);
    } finally {
      serialPortRef.current = null;
      setHardwareConnected(false);
      setIsSendingToHardware(false);
      setHardwareStatus("Hardware disconnected. Reconnect the Pico to resume OCR text handoff.");
    }
  };

  const sendBrailleToHardware = async (nextCells: BrailleCell[]) => {
    const port = serialPortRef.current;
    if (!port?.writable) {
      setHardwareError("Connect a Pico before sending OCR output.");
      setHardwareStatus("OCR is ready, but there is no connected hardware target.");
      return false;
    }

    if (nextCells.length === 0) {
      setHardwareError("No OCR output is ready to send yet.");
      return false;
    }

    setHardwareError("");
    setIsSendingToHardware(true);
    setSendState("sending");
    setHardwareStatus(`Synchronizing Pico timing and streaming ${nextCells.length} OCR-derived frames…`);

    try {
      const configReply = await sendHardwareCommand(
        port,
        `CONFIG:${hardwareCellDurationMs},${hardwareBlinkMs}\n`,
        "ACK CONFIG",
        4000,
      );
      setHardwareStatus(`Pico timing synced. ${configReply}. Sending pin frames now…`);

      const totalPlaybackMs = nextCells.length * hardwareCellDurationMs;
      await sendHardwareCommand(
        port,
        cellsToHardwarePayload(nextCells),
        "OK remote frames displayed",
        Math.max(5000, totalPlaybackMs + 5000),
      );
      setHardwareStatus(
        `Pico confirmed ${nextCells.length} pin frames at ${hardwareCellDurationMs}ms hold and ${hardwareBlinkMs}ms blink.`,
      );
      setSendState("sent");
      setSentTextPreview(recognizedText);
      return true;
    } catch (error) {
      const message = error instanceof Error ? error.message : "Serial write failed.";
      setHardwareError(`Hardware send failed: ${message}`);
      setHardwareStatus("OCR succeeded, but the direct hardware handoff failed.");
      setSendState("failed");
      return false;
    } finally {
      setIsSendingToHardware(false);
    }
  };

  const copyWokwiCommand = async (nextCells: BrailleCell[]) => {
    if (nextCells.length === 0) {
      setHardwareError("Scan a line before preparing the Wokwi simulator handoff.");
      return false;
    }

    const batchPayload = cellsToWokwiBatchPayload(
      nextCells,
      hardwareCellDurationMs,
      hardwareBlinkMs,
    );

    if (!navigator.clipboard?.writeText) {
      setHardwareError("Clipboard access is unavailable in this browser. Copy the Wokwi command from the payload panel.");
      setHardwareStatus("Wokwi command generated, but automatic copy is unavailable here.");
      return false;
    }

    try {
      await navigator.clipboard.writeText(batchPayload);
      setHardwareError("");
      setHardwareStatus("Wokwi command copied. Paste it into the Wokwi Serial Monitor and press Enter.");
      return true;
    } catch (error) {
      const message = error instanceof Error ? error.message : "Clipboard write failed.";
      setHardwareError(`Wokwi copy failed: ${message}`);
      setHardwareStatus("Wokwi command is ready below. Copy it manually into the Wokwi Serial Monitor.");
      return false;
    }
  };

  const sendToWokwiBridge = async (nextCells: BrailleCell[]) => {
    if (nextCells.length === 0) {
      setHardwareError("Scan a line before sending to the Wokwi simulator.");
      return false;
    }

    const batchPayload = `${cellsToWokwiBatchPayload(
      nextCells,
      hardwareCellDurationMs,
      hardwareBlinkMs,
    )}\n`;

    setHardwareError("");
    setHardwareStatus("Sending the simulator command to the local Wokwi bridge. Keep the Wokwi Serial Monitor focused.");
    setSendState("sending");

    try {
      const response = await fetch(WOKWI_BRIDGE_ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ text: batchPayload }),
      });

      const payload = await response.json() as { error?: string; message?: string };
      if (!response.ok) {
        throw new Error(payload.error ?? `Bridge returned ${response.status}`);
      }

      setHardwareStatus(payload.message ?? "Wokwi bridge delivered the command to the focused simulator window.");
      setSendState("sent");
      setSentTextPreview(recognizedText);
      return true;
    } catch (error) {
      const message = error instanceof Error ? error.message : "Local Wokwi bridge request failed.";
      setHardwareError(`Wokwi bridge failed: ${message}`);
      setHardwareStatus("Start the local bridge with `python run_project.py bridge`, or use Copy Wokwi command as a fallback.");
      setSendState("failed");
      return false;
    }
  };

  const scanLine = async () => {
    const video = videoRef.current;
    const canvas = captureCanvasRef.current;
    if (!video || !canvas || !cameraActive || video.videoWidth === 0) {
      setCameraError("Start the camera and wait for the live preview before scanning.");
      return;
    }

    setCameraError("");
    setIsScanning(true);
    setProgress(0);
    setOcrStatus("Capturing the highlighted line…");

    try {
      const crop = getGuideCrop(video);
      const scale = Math.min(2.5, 1800 / crop.width);
      const captureWidth = Math.max(900, Math.round(crop.width * scale));
      const captureHeight = Math.max(150, Math.round(crop.height * (captureWidth / crop.width)));
      const sourceCanvas = createCanvas(captureWidth, captureHeight);
      const sourceContext = sourceCanvas.getContext("2d", { willReadFrequently: true });
      if (!sourceContext) throw new Error("Unable to prepare the captured frame.");
      sourceContext.imageSmoothingEnabled = true;
      sourceContext.imageSmoothingQuality = "high";
      sourceContext.drawImage(
        video,
        crop.x,
        crop.y,
        crop.width,
        crop.height,
        0,
        0,
        sourceCanvas.width,
        sourceCanvas.height,
      );

      const variants = buildImageVariants(sourceCanvas);
      const worker = await getWorker();
      const candidates: OcrCandidate[] = [];
      ocrPassRef.current = { index: 0, total: OCR_VARIANTS };

      for (let index = 0; index < variants.length; index += 1) {
        ocrPassRef.current = { index, total: variants.length };
        const variant = variants[index];
        const result = await worker.recognize(variant.canvas);
        const text = cleanOcrText(result.data.text);
        candidates.push({
          canvas: variant.canvas,
          confidence: result.data.confidence,
          label: variant.label,
          score: scoreOcrResult(text, result.data.confidence),
          text,
        });
      }

      const best = candidates
        .filter((candidate) => candidate.text)
        .sort((a, b) => b.score - a.score)[0];
      if (!best) throw new Error("No clear printed text was detected. Re-align the line and scan again.");

      canvas.width = best.canvas.width;
      canvas.height = best.canvas.height;
      const previewContext = canvas.getContext("2d");
      if (!previewContext) throw new Error("Unable to show the enhanced scan.");
      previewContext.drawImage(best.canvas, 0, 0);

      const nextCells = textToBraille(best.text);

      if (nextCells.length === 0) {
        throw new Error("No clear printed text was detected. Re-align the line and scan again.");
      }

      setRecognizedText(best.text);
      setConfidence(Math.round(best.confidence));
      setCells(nextCells);
      setSendState("waiting");
      setSentTextPreview("");
      setProgress(100);
      setOcrStatus(
        best.confidence < 60
          ? "Low-confidence result. Check the text before sending to hardware."
          : `Local OCR selected the clearest of ${variants.length} enhanced scans. Ready to send.`,
      );

      if (handoffTarget === "pico" && hardwareConnected && autoSendToHardware) {
        await sendBrailleToHardware(nextCells);
      } else if (handoffTarget === "pico" && hardwareConnected) {
        setHardwareStatus("OCR ready. Use Send pin frames to push the result to the Pico.");
      } else if (handoffTarget === "wokwi") {
        if (autoSendToHardware) {
          const delivered = await sendToWokwiBridge(nextCells);
          if (!delivered) {
            setHardwareStatus("Bridge send failed. Use Send to focused Wokwi or Copy Wokwi command as fallback.");
          }
        } else {
          setHardwareStatus("OCR ready. Focus the Wokwi Serial Monitor, then use Send to focused Wokwi or Copy Wokwi command.");
        }
      } else {
        setHardwareStatus("OCR ready. Connect a Pico to stream direct pin frames.");
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : "OCR failed.";
      setCameraError(message);
      setOcrStatus("Scan unsuccessful. Adjust lighting and alignment, then try again.");
      setProgress(0);
    } finally {
      setIsScanning(false);
    }
  };

  const hardwareFrames = useMemo(
    () => cells.map((cell) => dotsToPinFrame(cell.dots)),
    [cells],
  );

  const hardwarePayloadPreview = useMemo(
    () => hardwareFrames.length ? `PINS:${hardwareFrames.join(",")}` : "",
    [hardwareFrames],
  );

  const wokwiBatchPreview = useMemo(
    () => hardwareFrames.length
      ? cellsToWokwiBatchPayload(cells, hardwareCellDurationMs, hardwareBlinkMs)
      : "",
    [cells, hardwareCellDurationMs, hardwareBlinkMs, hardwareFrames.length],
  );

  const sendStateLabel = sendState === "sent"
    ? "Sent"
    : sendState === "sending"
      ? "Sending"
      : sendState === "failed"
        ? "Send failed"
        : "Waiting to send";

  return (
    <main>
      <header className="site-header">
        <a className="brand" href="#top" aria-label="Micro-Tactile Reader home">
          <span className="brand-mark" aria-hidden="true">
            <i /><i /><i /><i /><i /><i />
          </span>
          <span>Micro-Tactile Reader</span>
        </a>
        <div className="prototype-chip"><span /> Hardware + software demo</div>
      </header>

      <section className="hero" id="top">
        <div>
          <p className="eyebrow">LIVE PRINTED-TEXT PROTOTYPE</p>
          <h1>From a printed line<br />to OCR text handoff, live.</h1>
          <p className="hero-copy">
            Point the camera at clear printed English text. OCR runs locally,
            then the recognized text is sent directly to the selected
            hardware target. The companion Pico/Wokwi display lives in the
            hardware workspace.
          </p>
        </div>
        <ol className="flow" aria-label="Prototype flow">
          <li><b>01</b><span>Align a line</span></li>
          <li><b>02</b><span>Run local OCR</span></li>
          <li><b>03</b><span>Mirror on hardware</span></li>
        </ol>
      </section>

      <section className="workspace" aria-label="OCR text handoff prototype">
        <article className="panel camera-panel">
          <div className="panel-heading">
            <div>
              <p className="step-label">01 · LIVE CAMERA</p>
              <h2>Align one printed line</h2>
            </div>
            <span className={`status-pill ${cameraActive ? "online" : ""}`}>
              {cameraActive ? "Camera live" : "Camera off"}
            </span>
          </div>

          <div className={`camera-stage ${cameraActive ? "is-live" : ""}`}>
            <video ref={videoRef} muted playsInline aria-label="Live camera preview" />
            {!cameraActive && (
              <div className="camera-empty">
                <div className="camera-icon" aria-hidden="true" />
                <strong>Camera is off</strong>
                <span>No image is recorded or uploaded.</span>
              </div>
            )}
            <div className="line-guide" aria-hidden="true">
              <span>KEEP ONE PRINTED LINE INSIDE THIS FRAME</span>
            </div>
            <div className="corner tl" /><div className="corner tr" />
            <div className="corner bl" /><div className="corner br" />
          </div>

          <canvas ref={captureCanvasRef} className="captured-line" aria-label="Captured OCR line" />

          <div className="camera-actions">
            <button className="button secondary" onClick={cameraActive ? stopCamera : startCamera}>
              {cameraActive ? "Stop camera" : "Start camera"}
            </button>
            <button className="button primary" onClick={scanLine} disabled={!cameraActive || isScanning}>
              {isScanning ? "Scanning…" : "Scan printed line"}
            </button>
          </div>

          <p className="privacy-note">
            <span aria-hidden="true">●</span> Private by design: frames and OCR stay in this browser.
          </p>
        </article>

        <article className="panel output-panel">
          <div className="panel-heading">
            <div>
              <p className="step-label">02 · OCR RESULT</p>
              <h2>Check before reading</h2>
            </div>
            {confidence !== null && (
              <span className={`confidence ${confidence < 60 ? "low" : ""}`}>
                {confidence}% confidence
              </span>
            )}
          </div>

          <div className="ocr-status" role="status" aria-live="polite">
            <span>{ocrStatus}</span>
            {(isScanning || progress > 0) && (
              <div className="progress-track" aria-label={`OCR progress ${progress}%`}>
                <i style={{ width: `${progress}%` }} />
              </div>
            )}
          </div>

          <div className={`recognized-text ${recognizedText ? "has-text" : ""}`}>
            <span>Recognized printed text</span>
            <p>{recognizedText || "Your scanned line will appear here."}</p>
          </div>

          <div className="recognized-text has-text">
            <span>Send state</span>
            <p>{sendStateLabel}{sentTextPreview ? ` · ${sentTextPreview}` : ""}</p>
          </div>

          {cameraError && <p className="error-message" role="alert">{cameraError}</p>}

          <div className="hardware-handoff">
            <div className="panel-heading compact">
              <div>
                <p className="step-label">02B · HARDWARE HANDOFF</p>
                <h2>Send OCR text directly</h2>
              </div>
              <span className={`status-pill ${(handoffTarget === "pico" && hardwareConnected) ? "online" : ""}`}>
                {handoffTarget === "wokwi" ? "Wokwi target" : hardwareConnected ? "Pico linked" : "No Pico"}
              </span>
            </div>

            <div className="target-toggle" aria-label="Hardware handoff target">
              <button
                className={handoffTarget === "wokwi" ? "active" : ""}
                onClick={() => {
                  setHandoffTarget("wokwi");
                  setHardwareStatus(
                    autoSendToHardware
                      ? "Wokwi mode is ready. Scan a line and it will auto-send to the focused Serial Monitor."
                      : "Wokwi mode is ready. Start the local bridge, focus the Serial Monitor, then send the simulator command.",
                  );
                  setHardwareError("");
                }}
              >
                Wokwi simulator
              </button>
              <button
                className={handoffTarget === "pico" ? "active" : ""}
                onClick={() => {
                  setHandoffTarget("pico");
                  setHardwareStatus(hardwareConnected
                    ? "Pico connected and ready for OCR text handoff."
                    : "Pico mode selected. Connect a board over USB to stream OCR-derived pin frames.");
                  setHardwareError("");
                }}
              >
                Real Pico USB
              </button>
            </div>

            <p className="hardware-status">{hardwareStatus}</p>
            {handoffTarget === "pico" && (
              <p className="hardware-reply">Latest Pico reply: {lastHardwareReply}</p>
            )}
            {handoffTarget === "wokwi" && (
              <p className="hardware-reply">Wokwi path: keep the Serial Monitor focused while scanning. Auto-send uses one `BATCH:` line per scan.</p>
            )}
            {hardwareError && <p className="error-message" role="alert">{hardwareError}</p>}

            <div className="hardware-actions">
              {handoffTarget === "pico" ? (
                <>
                  <button
                    className="button secondary"
                    onClick={hardwareConnected ? () => void disconnectHardware() : () => void connectHardware()}
                  >
                    {hardwareConnected ? "Disconnect Pico" : "Connect Pico"}
                  </button>
                  <button
                    className="button primary"
                    onClick={() => void sendBrailleToHardware(cells)}
                    disabled={!hardwareConnected || !cells.length || isSendingToHardware}
                  >
                    {isSendingToHardware ? "Sending…" : "Send pin frames"}
                  </button>
                </>
              ) : (
                <>
                  <button
                    className="button primary"
                    onClick={() => void sendToWokwiBridge(cells)}
                    disabled={!cells.length}
                  >
                    Send to focused Wokwi
                  </button>
                  <button
                    className="button secondary"
                    onClick={() => void copyWokwiCommand(cells)}
                    disabled={!cells.length}
                  >
                    Copy Wokwi command
                  </button>
                </>
              )}
            </div>

            <div className="timing-grid">
              <label>
                <span>Cell hold (ms)</span>
                <input
                  type="number"
                  min={150}
                  max={5000}
                  step={10}
                  value={hardwareCellDurationMs}
                  onChange={(event) => setManualHardwareCellDurationMs(Number(event.target.value) || DEFAULT_HARDWARE_CELL_DURATION_MS)}
                  disabled={syncHardwareTiming}
                />
              </label>
              <label>
                <span>Blink step (ms)</span>
                <input
                  type="number"
                  min={60}
                  max={2000}
                  step={10}
                  value={hardwareBlinkMs}
                  onChange={(event) => setManualHardwareBlinkMs(Number(event.target.value) || 250)}
                  disabled={syncHardwareTiming}
                />
              </label>
            </div>

            <label className="toggle-row">
              <input
                type="checkbox"
                checked={autoSendToHardware}
                onChange={(event) => setAutoSendToHardware(event.target.checked)}
              />
              <span>Auto-send after each successful scan to the selected hardware target</span>
            </label>

            <label className="toggle-row">
              <input
                type="checkbox"
                checked={syncHardwareTiming}
                onChange={(event) => setSyncHardwareTiming(event.target.checked)}
              />
              <span>Match current on-screen speed and push timing with `CONFIG:` before each send</span>
            </label>

            <div className={`bit-preview ${hardwarePayloadPreview ? "has-data" : ""}`}>
              <span>{handoffTarget === "wokwi" ? "Wokwi batch command" : "Pin-frame payload"}</span>
              <p>{handoffTarget === "wokwi"
                ? (wokwiBatchPreview || "BATCH payload will appear here after OCR converts the scanned text.")
                : (hardwarePayloadPreview || "PINS payload will appear here after OCR converts the scanned text.")}</p>
            </div>
          </div>

          <p className="privacy-note handoff-note">
            <span aria-hidden="true">●</span> Handoff modes: Web Serial at 115200 baud for a real Pico, or a local bridge that pastes one `BATCH:` line into the focused Wokwi Serial Monitor.
          </p>
        </article>
      </section>

      <section className="scope-section">
        <div>
          <p className="eyebrow">VALIDATION SCOPE</p>
          <h2>Small, honest and testable.</h2>
        </div>
        <div className="scope-grid">
          <article><b>Supported now</b><p>Clear printed English, one line at a time, under adequate lighting, with real-Pico streaming or Wokwi simulator batch handoff.</p></article>
          <article><b>Not yet supported</b><p>Handwriting, complex layouts, translation, summarisation or direct browser automation of a running Wokwi tab.</p></article>
          <article><b>Next validation</b><p>Drive the final physical pin actuator from the same serial protocol and add a richer simulator bridge if Wokwi exposes one.</p></article>
        </div>
      </section>

      <footer>
        <span>VASTOME · Tech4City 2026 Semi-Final</span>
        <span>Companion Pico demo included · No uploaded images</span>
      </footer>
    </main>
  );
}
