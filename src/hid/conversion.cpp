// Conversion of the reports of a BLE keyboard and of the lamps of the host (see conversion.h)

#include "conversion.h"

#include <string.h>

// Called with the usage of each key pressed in a report, in the order of the report
typedef void (*PressedKeyHandler)(uint16_t usage, void *context);

// Call the handler with the usage of each key pressed in a report, in the order of the report
static void readPressedKeys(const UsageFormat &format, uint8_t const *data, PressedKeyHandler handler, void *context) {
  for (uint8_t f = 0; f < format.fieldCount; f++) {
    const UsageField &field = format.fields[f];
    for (uint32_t i = 0; i < field.count; i++) {
      uint32_t value = readReportBits(data, field.bitOffset + i * field.bitSize, field.bitSize);
      uint16_t usage;
      if (field.array) {
        int32_t index = field.logicalMinimum < 0 ? signExtend(value, field.bitSize) : static_cast<int32_t>(value);
        // An out-of-range value is no key (HID 1.11, 6.2.2.5)
        if (index < field.logicalMinimum || index > field.logicalMaximum) {
          continue;
        }
        usage = fieldUsage(field, index - field.logicalMinimum);
      } else {
        if (value == 0) {
          continue;
        }
        usage = fieldUsage(field, i);
      }
      if (usage != 0) {
        handler(usage, context);
      }
    }
  }
}

// Read the keys of a keyboard input report into a boot keyboard report (see SPEC.md). False if the length is not that
// of the Report Map
bool readKeyboardReport(const UsageFormat &format, uint8_t const *data, size_t length, KeyboardReport &report) {
  if (!hasReportLength(format, length)) {
    return false;
  }
  // The pressed usages, one bit each
  uint8_t pressed[32] = {};
  readPressedKeys(format, data,
                  [](uint16_t usage, void *context) {
                    if (usage < 256) {
                      static_cast<uint8_t *>(context)[usage / 8] |= 1 << (usage % 8);
                    }
                  },
                  pressed);
  auto isPressed = [&pressed](unsigned usage) { return (pressed[usage / 8] & (1 << (usage % 8))) != 0; };

  report = {};
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (isPressed(USAGE_FIRST_MODIFIER + bit)) {
      report.modifiers |= 1 << bit;
    }
  }
  size_t count = 0;
  for (unsigned usage = USAGE_FIRST_KEY; usage <= USAGE_LAST_KEY; usage++) {
    if (isPressed(usage)) {
      if (count < KEYBOARD_REPORT_KEYS) {
        report.keys[count] = usage;
      }
      count++;
    }
  }
  // Too many keys at once: ErrorRollOver in every slot, with the modifiers still reported (HID 1.11, Appendix C). An
  // error that the keyboard reports is passed on in the same way
  uint8_t error = count > KEYBOARD_REPORT_KEYS ? USAGE_ERROR_ROLL_OVER : 0;
  for (uint8_t usage = USAGE_ERROR_ROLL_OVER; usage <= USAGE_ERROR_UNDEFINED; usage++) {
    if (isPressed(usage)) {
      error = usage;
      break;
    }
  }
  if (error != 0) {
    memset(report.keys, error, sizeof(report.keys));
  }
  return true;
}

// Whether the usage is among the usages
static bool containsUsage(uint16_t const *usages, uint8_t count, uint16_t usage) {
  for (uint8_t i = 0; i < count; i++) {
    if (usages[i] == usage) {
      return true;
    }
  }
  return false;
}

// Read the media keys pressed in a media key report: those that the host can be sent (see SPEC.md). False if the
// length is not that of the Report Map
bool readConsumerReport(const UsageFormat &format, uint8_t const *data, size_t length, PressedConsumerKeys &pressed) {
  if (!hasReportLength(format, length)) {
    return false;
  }
  pressed.count = 0;
  readPressedKeys(format, data,
                  [](uint16_t usage, void *context) {
                    PressedConsumerKeys &keys = *static_cast<PressedConsumerKeys *>(context);
                    if (usage <= USAGE_LAST_CONSUMER && keys.count < CONSUMER_MAX_PRESSED &&
                        !containsUsage(keys.usages, keys.count, usage)) {
                      keys.usages[keys.count++] = usage;
                    }
                  },
                  &pressed);
  return true;
}

// The media key to send after a media key report, as the guidelines of Microsoft (see SPEC.md): the key newly pressed
// (the first in the report if more than one), or none if the key sent has been released; otherwise the key sent
uint16_t chooseConsumerUsage(uint16_t sent, const PressedConsumerKeys &before, const PressedConsumerKeys &now) {
  for (uint8_t i = 0; i < now.count; i++) {
    if (!containsUsage(before.usages, before.count, now.usages[i])) {
      return now.usages[i];
    }
  }
  if (containsUsage(before.usages, before.count, sent) && !containsUsage(now.usages, now.count, sent)) {
    return 0;
  }
  return sent;
}

// Make a lamp output report from the lamps of the host (see SPEC.md): each lamp that the host sends, Num Lock to Kana,
// at its place, and 0 for the rest. Returns the length (0 if the report is longer than the buffer)
size_t makeLampReport(const UsageFormat &format, uint8_t lamps, uint8_t *data, size_t maxSize) {
  size_t length = (format.bitLength + 7) / 8;
  if (length == 0 || length > maxSize) {
    return 0;
  }
  memset(data, 0, length);
  auto isLit = [lamps](uint16_t usage) {
    return usage >= USAGE_FIRST_LAMP && usage <= USAGE_LAST_LAMP && (lamps & (1 << (usage - USAGE_FIRST_LAMP))) != 0;
  };
  for (uint8_t f = 0; f < format.fieldCount; f++) {
    const UsageField &field = format.fields[f];
    if (!field.array) {
      // Each element is a lamp: 1 if lit
      for (uint32_t i = 0; i < field.count; i++) {
        if (isLit(fieldUsage(field, i))) {
          writeReportBits(data, field.bitOffset + i * field.bitSize, field.bitSize, 1);
        }
      }
      continue;
    }
    // Each element holds the index of a lit lamp, in the order of the usages, while elements remain
    uint32_t element = 0;
    for (uint16_t usage = USAGE_FIRST_LAMP; usage <= USAGE_LAST_LAMP && element < field.count; usage++) {
      uint32_t index;
      if (!isLit(usage) || !fieldUsageIndex(field, usage, index)) {
        continue;
      }
      int64_t value = static_cast<int64_t>(field.logicalMinimum) + index;
      if (value > field.logicalMaximum) {
        continue;
      }
      writeReportBits(data, field.bitOffset + element * field.bitSize, field.bitSize, static_cast<uint32_t>(value));
      element++;
    }
  }
  return length;
}
