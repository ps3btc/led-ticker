// Scrolls a short message on a Modulino LED Matrix (ABX00152) until told to stop.
// Text arrives from the Linux side over the Router Bridge; frames are advanced
// manually because ModulinoLEDMatrix::play() blocks and would starve Stop.

#include <ArduinoGraphics.h>
#include <Arduino_Modulino.h>
#include <Modulino_LED_Matrix.h>
#include <Arduino_RouterBridge.h>

constexpr uint8_t MAX_CHARS = 16;      // must match MAX_CHARS in python/main.py
constexpr size_t MAX_FRAMES = 128;     // 128 x 16 B = 2 KB; 16 chars at Font_4x6 needs ~92
constexpr int DEFAULT_MS = 240;        // ms per pixel of travel (higher = slower)
constexpr int MIN_MS = 40;             // must match the slider in assets/index.html
constexpr int MAX_MS = 1200;

ModulinoLEDMatrix matrix;

uint8_t frames[MAX_FRAMES][MONOCHROMATIC_ANIMATION_FRAME_SIZE];
uint32_t framesUsed = 0;

// Touched by the Bridge thread, consumed by loop().
volatile bool showPending = false;
volatile bool stopPending = false;
volatile bool running = false;
volatile bool matrixPresent = false;
volatile unsigned long speedMs = DEFAULT_MS;
char requested[MAX_CHARS + 1];

unsigned long lastFrameAt = 0;
bool warnedAbsent = false;

void setSpeed(int ms) {
  if (ms < MIN_MS) ms = MIN_MS;
  if (ms > MAX_MS) ms = MAX_MS;
  speedMs = ms;
}

void setText(String text) {
  uint8_t kept = 0;
  for (uint16_t i = 0; i < text.length() && kept < MAX_CHARS; i++) {
    char c = text[i];
    if (c >= ' ' && c < 0x7F) {
      requested[kept++] = c;
    }
  }
  requested[kept] = '\0';
  if (kept == 0) {
    return;
  }
  showPending = true;
}

void stopText() {
  showPending = false;
  stopPending = true;
}

bool matrixReady() {
  return matrixPresent;
}

void buildSequence() {
  String padded = "  " + String(requested) + "  ";

  matrix.textScrollSpeed(speedMs);
  matrix.textFont(Font_4x6);
  matrix.beginText(0, 1, 0xFFFFFF);
  matrix.print(padded);

  framesUsed = 0;
  matrix.endTextAnimation(SCROLL_LEFT, frames, framesUsed);

  if (framesUsed == 0) {
    Serial.println(F("ticker: capture produced no frames"));
    return;
  }
  if (framesUsed == sizeof(frames)) {
    Serial.println(F("ticker: buffer full, animation truncated (shorten the text)"));
  }

  matrix.setSequence(frames, framesUsed);
  lastFrameAt = millis();
  running = true;
}

void setup() {
  Bridge.begin();
  Modulino.begin(Wire1);

  for (uint8_t attempt = 0; attempt < 3 && !matrixPresent; attempt++) {
    matrixPresent = matrix.begin() == 1;
    if (!matrixPresent) {
      delay(100);
    }
  }
  Serial.print(F("ticker: matrix ready="));
  Serial.println(matrixPresent);

  Bridge.provide("set_text", setText);
  Bridge.provide("set_speed", setSpeed);
  Bridge.provide("stop_text", stopText);
  Bridge.provide("matrix_ready", matrixReady);

  if (matrixPresent) {
    matrix.clear();
  }
}

void loop() {
  if (!matrixPresent) {
    if (showPending || stopPending) {
      showPending = false;
      stopPending = false;
      if (!warnedAbsent) {
        warnedAbsent = true;
        Serial.println(F("ticker: request ignored, no matrix answering on Wire1"));
      }
    }
    return;
  }

  if (stopPending) {
    stopPending = false;
    running = false;
    matrix.clear();
    Serial.println(F("ticker: stopped"));
  }

  if (showPending) {
    showPending = false;
    buildSequence();
  }

  if (running) {
    if (millis() - lastFrameAt >= speedMs) {
      lastFrameAt = millis();
      matrix.nextFrame();
    }
  }
}
