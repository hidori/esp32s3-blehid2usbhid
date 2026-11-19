// Conversion of the reports of a BLE keyboard into the reports sent to the host, and of the lamps of the host into a
// lamp output report of the BLE keyboard (see SPEC.md). Uses neither Arduino nor BLE, so that it can be tested on a PC

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_map.h"

// Usages of the HID Keyboard/Keypad page: the errors (ErrorRollOver, POSTFail, ErrorUndefined), the keys, and the
// modifiers 0xE0-0xE7, which go in the modifier byte
const uint8_t USAGE_ERROR_ROLL_OVER = 0x01;
const uint8_t USAGE_ERROR_UNDEFINED = 0x03;
const uint8_t USAGE_FIRST_KEY = 0x04;
const uint8_t USAGE_LAST_KEY = 0xDF;
const uint8_t USAGE_FIRST_MODIFIER = 0xE0;

// Usages of the HID Consumer page (media keys): those that the consumer control collection sends (see SPEC.md)
const uint16_t USAGE_LAST_CONSUMER = 0x3FF;

// Usages of the HID LED page (lamps): those that the host sends to a boot keyboard, Num Lock (bit 0) to Kana (bit 4)
// (see SPEC.md)
const uint16_t USAGE_FIRST_LAMP = 0x01;
const uint16_t USAGE_LAST_LAMP = 0x05;

// Keys in a boot keyboard report, and the media keys pressed at once that are tracked
const size_t KEYBOARD_REPORT_KEYS = 6;
const size_t CONSUMER_MAX_PRESSED = 16;

// Boot keyboard report
struct KeyboardReport {
  uint8_t modifiers;
  uint8_t keys[KEYBOARD_REPORT_KEYS];
};

// Media keys pressed in a media key report, in the order of the report
struct PressedConsumerKeys {
  uint8_t count;
  uint16_t usages[CONSUMER_MAX_PRESSED];
};

// Read the keys of a keyboard input report into a boot keyboard report. False if the length is not that of the Report
// Map
bool readKeyboardReport(const UsageFormat &format, uint8_t const *data, size_t length, KeyboardReport &report);

// Read the media keys pressed in a media key report: those that the host can be sent. False if the length is not that
// of the Report Map
bool readConsumerReport(const UsageFormat &format, uint8_t const *data, size_t length, PressedConsumerKeys &pressed);

// The media key to send after a media key report, as the guidelines of Microsoft
uint16_t chooseConsumerUsage(uint16_t sent, const PressedConsumerKeys &before, const PressedConsumerKeys &now);

// Make a lamp output report from the lamps of the host. Returns the length (0 if the report is longer than the buffer)
size_t makeLampReport(const UsageFormat &format, uint8_t lamps, uint8_t *data, size_t maxSize);
