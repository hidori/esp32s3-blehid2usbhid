// Status LED (see led.h)

#include "led.h"

#include <Arduino.h>

#include "../ble/ble.h"

// GPIO of the RGB LED (WS2812) of AtomS3 Lite and AtomS3U (see SPEC.md). The board definition of the build names
// another pin as RGB_BUILTIN, so the pin is given here
static const uint8_t LED_PIN = 35;

// Brightness of a color (0-255): a WS2812 dazzles at full brightness
static const uint8_t LED_LEVEL = 32;

// Periods of the blinking, lit for the first half (see SPEC.md)
static const uint32_t SLOW_BLINK_MS = 1000;
static const uint32_t FAST_BLINK_MS = 250;

// Interval of looking at the state of the bridge: the bonds are read to see whether a device is registered, which is
// not worth doing in every loop()
static const uint32_t UPDATE_INTERVAL_MS = 20;

// What the LED shows: a color, lit or blinking
struct LedPattern {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint32_t blinkMs;  // Period of the blinking (0: lit)
};

// The patterns of SPEC.md
static const LedPattern ERROR_PATTERN = {LED_LEVEL, 0, 0, SLOW_BLINK_MS};             // Red, blinking
static const LedPattern PAIRING_PATTERN = {0, 0, LED_LEVEL, FAST_BLINK_MS};           // Blue, blinking fast
static const LedPattern WAITING_PATTERN = {0, LED_LEVEL, 0, SLOW_BLINK_MS};           // Green, blinking slowly
static const LedPattern CONNECTED_PATTERN = {0, LED_LEVEL, 0, 0};                     // Green, lit
static const LedPattern UNREGISTERED_PATTERN = {LED_LEVEL, LED_LEVEL * 3 / 4, 0, 0};  // Yellow, lit

// Color written last, as 0xRRGGBB (written again only when it changes; none at first)
static uint32_t writtenColor = 0xFFFFFFFF;
static uint32_t lastUpdateMs;

// Light the LED in the color (0, 0, 0 turns it off)
static void writeColor(uint8_t red, uint8_t green, uint8_t blue) {
  uint32_t color = static_cast<uint32_t>(red) << 16 | static_cast<uint32_t>(green) << 8 | blue;
  if (color == writtenColor) {
    return;
  }
  rgbLedWrite(LED_PIN, red, green, blue);
  writtenColor = color;
}

// The pattern of the state of the bridge: the first one in the table of SPEC.md that applies. A device is connected
// when it is relayed (being prepared after connecting, it still is waiting)
static const LedPattern &currentPattern() {
  if (hasBleFailed()) {
    return ERROR_PATTERN;
  }
  if (isScanning() || isPairing()) {
    return PAIRING_PATTERN;
  }
  EspBleBond registered;
  if (!readRegisteredKeyboard(registered)) {
    return UNREGISTERED_PATTERN;
  }
  return keyboardState() == KEYBOARD_RELAYING ? CONNECTED_PATTERN : WAITING_PATTERN;
}

void setupLed() {
  writeColor(0, 0, 0);
}

void updateLed() {
  uint32_t now = millis();
  if (now - lastUpdateMs < UPDATE_INTERVAL_MS) {
    return;
  }
  lastUpdateMs = now;
  const LedPattern &pattern = currentPattern();
  bool lit = pattern.blinkMs == 0 || now % pattern.blinkMs < pattern.blinkMs / 2;
  if (lit) {
    writeColor(pattern.red, pattern.green, pattern.blue);
  } else {
    writeColor(0, 0, 0);
  }
}
