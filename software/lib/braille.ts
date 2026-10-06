export type BrailleCell = {
  dots: number[];
  label: string;
  source: string;
  unicode: string;
};

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
  "\"": [5], "(": [1, 2, 6], ")": [3, 4, 5], "/": [3, 4],
};

const DIGIT_LETTERS: Record<string, string> = {
  "1": "a", "2": "b", "3": "c", "4": "d", "5": "e",
  "6": "f", "7": "g", "8": "h", "9": "i", "0": "j",
};

function makeCell(dots: number[], label: string, source: string): BrailleCell {
  const bits = dots.reduce((sum, dot) => sum + (1 << (dot - 1)), 0);
  return { dots, label, source, unicode: String.fromCodePoint(0x2800 + bits) };
}

// Pico's transport remains 8 bits wide; the six-dot reader leaves dots 7-8 off.
export function dotsToPinFrame(dots: number[]): string {
  return Array.from({ length: 8 }, (_, index) => (
    dots.includes(index + 1) ? "1" : "0"
  )).join("");
}

export function cellsToHardwarePayload(cells: BrailleCell[]): string {
  return `PINS:${cells.map((cell) => dotsToPinFrame(cell.dots)).join(",")}\n`;
}

export function cellsToWokwiBatchPayload(
  cells: BrailleCell[], holdMs: number, blinkMs: number,
): string {
  return `BATCH:${holdMs},${blinkMs}|${cells.map((cell) => dotsToPinFrame(cell.dots)).join(",")}`;
}

// Mirrors edge/BrailleEncoder: English six-dot cells, individual capital signs,
// and one number sign for each uninterrupted run of digits.
export function textToBraille(text: string): BrailleCell[] {
  const cells: BrailleCell[] = [];
  let numberMode = false;
  for (const character of text) {
    const digit = DIGIT_LETTERS[character];
    if (digit) {
      if (!numberMode) cells.push(makeCell([3, 4, 5, 6], "number sign", character));
      numberMode = true;
      cells.push(makeCell(LETTER_DOTS[digit], `digit ${character}`, character));
      continue;
    }
    numberMode = false;
    if (character === " ") {
      cells.push(makeCell([], "space", character));
      continue;
    }
    if (character >= "A" && character <= "Z") {
      cells.push(makeCell([6], "capital sign", character));
    }
    const lower = character.toLowerCase();
    const dots = LETTER_DOTS[lower] ?? PUNCTUATION_DOTS[character];
    cells.push(makeCell(dots ?? PUNCTUATION_DOTS["?"], dots ? character : "?", character));
  }
  return cells;
}
