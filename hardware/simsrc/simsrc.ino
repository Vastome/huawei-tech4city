/*
  Dynamic 8-Dot Braille Display with I2C LCD Highlight
  (Raspberry Pi Pico / RP2040 version — Wokwi simulation)
  -----------------------------------------------------------------
  Matches this diagram.json exactly:
    - wokwi-pi-pico  "pico"
    - wokwi-max7219-matrix "matrix1": chain=1
    - wokwi-lcd1602  "lcd1": pins="i2c" (PCF8574 @ 0x27 default)
    - GP0 -> $serialMonitor:RX, GP1 -> $serialMonitor:TX
      => Pico UART0 is wired to the Wokwi Serial Monitor, so this
         sketch uses Serial1 (arduino-pico core: UART0, TX=GP0,
         RX=GP1), NOT the USB "Serial" object.

  WIRING (already in diagram.json)
  -------
  matrix1 DIN -> pico GP19
  matrix1 CLK -> pico GP18
  matrix1 CS  -> pico GP17
  matrix1 VCC -> pico 3V3
  matrix1 GND -> pico GND.3
  lcd1   SDA  -> pico GP4
  lcd1   SCL  -> pico GP5
  lcd1   VCC  -> pico 3V3
  lcd1   GND  -> pico GND.2
  pico GP0 -> $serialMonitor:RX
  pico GP1 -> $serialMonitor:TX

  Dot numbering used (8-dot / computer braille):
      1 4
      2 5
      3 6
      7 8
  Bit0 = dot1 (LSB) ... Bit7 = dot8 (MSB) in the byte pattern below.

  The MAX7219 matrix is an 8x8 display, but the braille cells only
  use the top-left 4x2 area so the layout stays visually consistent
  with braille while keeping the wiring simple.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- USER CONFIG ----------
#define LCD_ADDR   0x27   // PCF8574-based, Address: 39 (0x27)
#define LCD_COLS   16
#define LCD_ROWS   2

#define DOT_MATRIX_DIN  19  // GP19 -> DIN of the MAX7219 dot matrix
#define DOT_MATRIX_CLK  18  // GP18 -> CLK of the MAX7219 dot matrix
#define DOT_MATRIX_CS   17  // GP17 -> CS  of the MAX7219 dot matrix

const unsigned long DEFAULT_LETTER_DISPLAY_MS = 900; // time each letter is shown
const unsigned long DEFAULT_BLINK_MS = 250;          // matrix on/off blink period

// Color used for an "on" dot (dim white keeps it comfortable in sim/real use)
const uint32_t DOT_COLOR_ON  = 0xFFFFFF; // R,G,B packed
const uint32_t DOT_COLOR_OFF = 0x000000;

// GP0/GP1 are wired to the Wokwi Serial Monitor via UART0 in
// diagram.json, so we keep Serial1 for Wokwi while also listening on
// USB Serial for the browser handoff path used on real hardware.
#define SerialPort Serial1
// ----------------------------------

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
uint8_t dotMatrixFrame[8] = {0};

String inputText = "";
bool newTextReady = false;
String uartInputBuffer;
String usbInputBuffer;
unsigned long remoteLetterDisplayMs = DEFAULT_LETTER_DISPLAY_MS;
unsigned long remoteBlinkMs = DEFAULT_BLINK_MS;

// ---- 8-dot Braille lookup table (dots 1-6 = standard braille, 7-8 unused = 0) ----
struct BrailleMap {
  char ch;
  byte pattern;
};

const BrailleMap brailleTable[] = {
  {'a', 0b00000001}, {'b', 0b00000011}, {'c', 0b00001001}, {'d', 0b00011001},
  {'e', 0b00010001}, {'f', 0b00001011}, {'g', 0b00011011}, {'h', 0b00010011},
  {'i', 0b00001010}, {'j', 0b00011010}, {'k', 0b00000101}, {'l', 0b00000111},
  {'m', 0b00001101}, {'n', 0b00011101}, {'o', 0b00010101}, {'p', 0b00001111},
  {'q', 0b00011111}, {'r', 0b00010111}, {'s', 0b00001110}, {'t', 0b00011110},
  {'u', 0b00100101}, {'v', 0b00100111}, {'w', 0b00111010}, {'x', 0b00101101},
  {'y', 0b00111101}, {'z', 0b00110101},
  {'0', 0b00110101}, {'1', 0b00000001}, {'2', 0b00000011}, {'3', 0b00001001},
  {'4', 0b00011001}, {'5', 0b00010001}, {'6', 0b00001011}, {'7', 0b00011011},
  {'8', 0b00010011}, {'9', 0b00001010},
  {' ', 0b00000000}, {'.', 0b00110010}, {',', 0b00000010}, {'!', 0b00101011},
  {'?', 0b00101010}, {'-', 0b00100100}, {'\'', 0b00000100}
};
const int brailleTableSize = sizeof(brailleTable) / sizeof(BrailleMap);

void dotMatrixFlush();
void announceReady(Print &port);
void handleSerialInput(Stream &input, Print &output, String &buffer);
bool handleCommandLine(const String &line, Print &output);
bool applyRemoteTiming(const String &payload, Print &output);
bool processBatchCommand(const String &payload, Print &output);
bool parsePinToken(const String &token, byte &pattern);
void processRemoteFrames(const String &payload, Print &output);
void updateRemoteFrameStatus(int currentIndex, int totalFrames);

byte getBraillePattern(char c) {
  char lc = tolower(c);
  for (int i = 0; i < brailleTableSize; i++) {
    if (brailleTable[i].ch == lc) return brailleTable[i].pattern;
  }
  return 0b00000000; // unknown char -> blank cell
}

void allDotsOff() {
  memset(dotMatrixFrame, 0, sizeof(dotMatrixFrame));
  dotMatrixFlush();
}

void dotMatrixWrite(uint8_t registerAddress, uint8_t value) {
  digitalWrite(DOT_MATRIX_CS, LOW);
  shiftOut(DOT_MATRIX_DIN, DOT_MATRIX_CLK, MSBFIRST, registerAddress);
  shiftOut(DOT_MATRIX_DIN, DOT_MATRIX_CLK, MSBFIRST, value);
  digitalWrite(DOT_MATRIX_CS, HIGH);
}

void dotMatrixFlush() {
  for (uint8_t row = 0; row < 8; row++) {
    dotMatrixWrite(row + 1, dotMatrixFrame[row]);
  }
}

void dotMatrixSetPixel(uint8_t x, uint8_t y, bool on) {
  if (x >= 8 || y >= 8) return;

  // Wokwi's current MAX7219 module is mirrored horizontally, so X=0 must map
  // to the leftmost visible LED in the row.
  uint8_t mask = 1 << x;
  if (on) {
    dotMatrixFrame[y] |= mask;
  } else {
    dotMatrixFrame[y] &= ~mask;
  }
}

void showPatternOnDotMatrix(byte pattern) {
  memset(dotMatrixFrame, 0, sizeof(dotMatrixFrame));

  const uint8_t dotX[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
  const uint8_t dotY[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };

  for (uint8_t i = 0; i < 8; i++) {
    bool on = (pattern >> i) & 0x01;
    dotMatrixSetPixel(dotX[i], dotY[i], on);
  }

  dotMatrixFlush();
}

// Shows the text with [brackets] highlighting the active letter,
// scrolling the visible window on a 16-col LCD as needed.
void updateLcdHighlight(const String &text, int activeIndex) {
  String display = text.substring(0, activeIndex) + "[" + text.charAt(activeIndex) + "]" + text.substring(activeIndex + 1);

  int taggedActivePos = activeIndex + 1; // shift due to inserted '['
  int windowStart = 0;
  if (display.length() > LCD_COLS) {
    windowStart = taggedActivePos - (LCD_COLS / 2);
    if (windowStart < 0) windowStart = 0;
    if (windowStart > (int)display.length() - LCD_COLS) {
      windowStart = display.length() - LCD_COLS;
    }
  }

  String visible = display.substring(windowStart, min((int)display.length(), windowStart + LCD_COLS));

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(visible);
}

void playBraille(byte pattern, unsigned long displayMs, unsigned long blinkMs) {
  unsigned long start = millis();
  bool on = false;
  while (millis() - start < displayMs) {
    on = !on;
    if (on) {
      showPatternOnDotMatrix(pattern);
    } else {
      allDotsOff();
    }
    delay(blinkMs);
  }
  allDotsOff();
}

void processText(const String &text) {
  for (int i = 0; i < (int)text.length(); i++) {
    char c = text.charAt(i);
    byte pattern = getBraillePattern(c);

    updateLcdHighlight(text, i);
    playBraille(pattern, DEFAULT_LETTER_DISPLAY_MS, DEFAULT_BLINK_MS);
  }
}

void announceReady(Print &port) {
  port.println("Braille LED Display ready (Pico).");
  port.println("Send text, or send PINS:10100000,11000000 frames.");
}

void updateRemoteFrameStatus(int currentIndex, int totalFrames) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Remote braille");
  lcd.setCursor(0, 1);
  lcd.print("Frame ");
  lcd.print(currentIndex);
  lcd.print("/");
  lcd.print(totalFrames);
}

bool applyRemoteTiming(const String &payload, Print &output) {
  int separator = payload.indexOf(',');
  if (separator <= 0 || separator >= payload.length() - 1) {
    output.println("ERR invalid CONFIG payload");
    return false;
  }

  String holdToken = payload.substring(0, separator);
  String blinkToken = payload.substring(separator + 1);
  holdToken.trim();
  blinkToken.trim();

  unsigned long nextHold = holdToken.toInt();
  unsigned long nextBlink = blinkToken.toInt();
  if (nextHold < 150 || nextHold > 5000 || nextBlink < 60 || nextBlink > 2000) {
    output.println("ERR CONFIG out of range");
    return false;
  }

  remoteLetterDisplayMs = nextHold;
  remoteBlinkMs = nextBlink;

  output.print("ACK CONFIG ");
  output.print(remoteLetterDisplayMs);
  output.print(" ");
  output.println(remoteBlinkMs);
  return true;
}

bool processBatchCommand(const String &payload, Print &output) {
  int separator = payload.indexOf('|');
  if (separator <= 0 || separator >= payload.length() - 1) {
    output.println("ERR invalid BATCH payload");
    return false;
  }

  String configPayload = payload.substring(0, separator);
  String framesPayload = payload.substring(separator + 1);

  if (!applyRemoteTiming(configPayload, output)) {
    return false;
  }

  processRemoteFrames(framesPayload, output);
  return true;
}

bool parsePinToken(const String &token, byte &pattern) {
  if (token.length() != 8) {
    return false;
  }

  pattern = 0;
  for (int index = 0; index < 8; index++) {
    char bit = token.charAt(index);
    if (bit != '0' && bit != '1') {
      return false;
    }
    if (bit == '1') {
      pattern |= (1 << index);
    }
  }

  return true;
}

void processRemoteFrames(const String &payload, Print &output) {
  int totalFrames = 1;
  for (int index = 0; index < (int)payload.length(); index++) {
    if (payload.charAt(index) == ',') {
      totalFrames += 1;
    }
  }

  int frameIndex = 0;
  int start = 0;
  while (start <= (int)payload.length()) {
    int separator = payload.indexOf(',', start);
    String token = separator == -1 ? payload.substring(start) : payload.substring(start, separator);
    token.trim();

    byte pattern = 0;
    if (!parsePinToken(token, pattern)) {
      output.println("ERR invalid PINS payload");
      return;
    }

    frameIndex += 1;
    output.print("FRAME ");
    output.print(frameIndex);
    output.print("/");
    output.println(totalFrames);
    updateRemoteFrameStatus(frameIndex, totalFrames);
    playBraille(pattern, remoteLetterDisplayMs, remoteBlinkMs);

    if (separator == -1) {
      break;
    }
    start = separator + 1;
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Remote done");
  lcd.setCursor(0, 1);
  lcd.print("Awaiting next");
  output.println("OK remote frames displayed");
}

bool handleCommandLine(const String &line, Print &output) {
  if (line.startsWith("BATCH:")) {
    return processBatchCommand(line.substring(6), output);
  }

  if (line.startsWith("CONFIG:")) {
    return applyRemoteTiming(line.substring(7), output);
  }

  if (!line.startsWith("PINS:")) {
    return false;
  }

  processRemoteFrames(line.substring(5), output);
  return true;
}

void handleSerialInput(Stream &input, Print &output, String &buffer) {
  while (input.available()) {
    char c = input.read();
    if (c == '\n' || c == '\r') {
      if (buffer.length() == 0) {
        continue;
      }

      if (!handleCommandLine(buffer, output)) {
        inputText = buffer;
        newTextReady = true;
      }
      buffer = "";
      continue;
    }

    buffer += c;
  }
}

void setup() {
  pinMode(DOT_MATRIX_CS, OUTPUT);
  pinMode(DOT_MATRIX_CLK, OUTPUT);
  pinMode(DOT_MATRIX_DIN, OUTPUT);
  digitalWrite(DOT_MATRIX_CS, HIGH);

  Serial.begin(115200);
  SerialPort.begin(115200);

  dotMatrixWrite(0x0F, 0x00); // display test off
  dotMatrixWrite(0x0C, 0x01); // normal operation
  dotMatrixWrite(0x0B, 0x07); // scan all 8 rows
  dotMatrixWrite(0x09, 0x00); // no decode mode
  dotMatrixWrite(0x0A, 0x08); // medium brightness
  allDotsOff();

  // I2C pins for the Pico's default Wire bus (GP4=SDA, GP5=SCL)
  Wire.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Braille LED Demo");
  lcd.setCursor(0, 1);
  lcd.print("Ready for text");
  delay(1500);

  announceReady(Serial);
  announceReady(SerialPort);

  newTextReady = false;
}

void loop() {
  handleSerialInput(Serial, Serial, usbInputBuffer);
  handleSerialInput(SerialPort, SerialPort, uartInputBuffer);

  if (newTextReady) {
    Serial.print("Displaying: ");
    Serial.println(inputText);
    SerialPort.print("Displaying: ");
    SerialPort.println(inputText);
    processText(inputText);
    newTextReady = false;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Done. Send next");
    lcd.setCursor(0, 1);
    lcd.print("word/sentence.");

    inputText = "";
  }
}
