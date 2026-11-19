// Layout of the keys and the lamps in the reports of a BLE keyboard, read from its Report Map (see report_map.h)

#include "report_map.h"

#include <string.h>

#include <algorithm>

// Value of a field of a HID report, least significant bit first
uint32_t readReportBits(uint8_t const *data, uint32_t bitOffset, uint8_t bitSize) {
  uint32_t value = 0;
  for (uint8_t bit = 0; bit < bitSize; bit++) {
    uint32_t source = bitOffset + bit;
    if (data[source / 8] & (1 << (source % 8))) {
      value |= 1UL << bit;
    }
  }
  return value;
}

// Value of a two's complement number of the size in bits
int32_t signExtend(uint32_t value, uint8_t bits) {
  if (bits == 0 || bits >= 32) {
    return static_cast<int32_t>(value);
  }
  uint32_t sign = 1UL << (bits - 1);
  return static_cast<int32_t>((value ^ sign) - sign);
}

// Write a field of a HID report, least significant bit first
void writeReportBits(uint8_t *data, uint32_t bitOffset, uint8_t bitSize, uint32_t value) {
  for (uint8_t bit = 0; bit < bitSize; bit++) {
    uint32_t target = bitOffset + bit;
    if (value & (1UL << bit)) {
      data[target / 8] |= 1 << (target % 8);
    } else {
      data[target / 8] &= ~(1 << (target % 8));
    }
  }
}

// Read the layout of the reports of the main item (input or output) from a Report Map (HID 1.11, 6.2.2): the fields of
// the usages on the page in the application collections of the usage (the keys of a keyboard: Keyboard/Keypad page,
// the media keys of a consumer control: Consumer page, the lamps of a keyboard: LED page). Returns the number of layouts
size_t parseUsageFormats(uint8_t const *map, size_t size, uint32_t collection, uint16_t usagePage, uint8_t item,
                         UsageFormat *formats, size_t maxFormats) {
  // Global items, which Push and Pop save and restore
  struct Globals {
    uint16_t usagePage;
    uint32_t logicalMinimum;
    uint8_t logicalMinimumSize;
    uint32_t logicalMaximum;
    uint8_t logicalMaximumSize;
    uint32_t reportSize;
    uint32_t reportCount;
    uint8_t reportId;
  };
  const size_t MAX_PUSHED = 4;
  Globals globals = {};
  Globals pushed[MAX_PUSHED];
  size_t pushedCount = 0;

  // Local items. A usage of 4 bytes holds its page in the upper half; a shorter one is on the Usage Page in effect at
  // the main item (HID 1.11, 6.2.2.8)
  struct Usage {
    uint32_t value;
    bool extended;
  };
  Usage usages[FIELD_MAX_USAGES];
  size_t usageCount = 0;
  bool usagesCut = false;
  Usage usageMinimum = {};
  Usage usageMaximum = {};
  bool usageRange = false;
  auto fullUsage = [&globals](const Usage &usage) {
    return usage.extended ? usage.value : (static_cast<uint32_t>(globals.usagePage) << 16) | usage.value;
  };

  // Whether each open collection is in an application collection of the usage
  const size_t MAX_DEPTH = 8;
  bool targetCollection[MAX_DEPTH] = {};
  size_t depth = 0;
  auto inTarget = [&]() { return depth > 0 && targetCollection[std::min(depth, MAX_DEPTH) - 1]; };

  // Length of each input report so far, in bits
  const size_t MAX_REPORT_IDS = 16;
  const uint32_t MAX_REPORT_BITS = 0xFFFF;
  uint8_t reportIds[MAX_REPORT_IDS];
  uint32_t reportBits[MAX_REPORT_IDS];
  size_t reportIdCount = 0;

  size_t formatCount = 0;
  for (size_t offset = 0; offset < size;) {
    uint8_t prefix = map[offset++];
    // Long item (none is defined): skip it
    if (prefix == 0xFE) {
      if (offset + 2 > size) {
        break;
      }
      offset += 2 + map[offset];
      continue;
    }
    uint8_t dataSize = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
    if (offset + dataSize > size) {
      break;
    }
    uint32_t data = 0;
    for (uint8_t i = 0; i < dataSize; i++) {
      data |= static_cast<uint32_t>(map[offset + i]) << (8 * i);
    }
    offset += dataSize;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = prefix >> 4;

    if (type == 1) {  // Global
      switch (tag) {
        case 0: globals.usagePage = data; break;
        case 1: globals.logicalMinimum = data; globals.logicalMinimumSize = dataSize; break;
        case 2: globals.logicalMaximum = data; globals.logicalMaximumSize = dataSize; break;
        case 7: globals.reportSize = data; break;
        case 8: globals.reportId = data; break;
        case 9: globals.reportCount = data; break;
        case 10: if (pushedCount < MAX_PUSHED) pushed[pushedCount++] = globals; break;  // Push
        case 11: if (pushedCount > 0) globals = pushed[--pushedCount]; break;          // Pop
      }
      continue;
    }
    if (type == 2) {  // Local
      Usage usage = {data, dataSize == 4};
      if (tag == 0) {
        if (usageCount < FIELD_MAX_USAGES) {
          usages[usageCount++] = usage;
        } else {
          usagesCut = true;
        }
      } else if (tag == 1) {
        usageMinimum = usage;
        usageRange = true;
      } else if (tag == 2) {
        usageMaximum = usage;
      }
      continue;
    }
    if (type != 0) {
      continue;
    }

    // Main
    if (tag == 10) {  // Collection
      // Application collection (1) of the usage
      bool target = inTarget() || (data == 1 && usageCount > 0 && fullUsage(usages[0]) == collection);
      if (depth < MAX_DEPTH) {
        targetCollection[depth] = target;
      }
      depth++;
    } else if (tag == 12) {  // End Collection
      if (depth > 0) {
        depth--;
      }
    } else if (tag == item) {  // Input or Output, as asked
      // Where this item starts in its report
      size_t report = 0;
      while (report < reportIdCount && reportIds[report] != globals.reportId) {
        report++;
      }
      if (report == reportIdCount && reportIdCount < MAX_REPORT_IDS) {
        reportIds[reportIdCount] = globals.reportId;
        reportBits[reportIdCount++] = 0;
      }
      if (report < reportIdCount) {
        uint32_t bitOffset = reportBits[report];
        uint32_t itemBits = globals.reportSize <= 32 && globals.reportCount <= MAX_REPORT_BITS
                                ? globals.reportSize * globals.reportCount
                                : MAX_REPORT_BITS + 1;
        reportBits[report] = std::min(bitOffset + itemBits, MAX_REPORT_BITS + 1);

        // Keys or lamps: data (bit 0 clear) in a collection of the usage, on the page asked
        uint32_t firstUsage = usageRange ? fullUsage(usageMinimum) : usageCount > 0 ? fullUsage(usages[0]) : 0;
        UsageFormat *format = NULL;
        if (inTarget() && (data & 0x01) == 0 && (firstUsage >> 16) == usagePage && globals.reportSize >= 1 &&
            globals.reportCount >= 1 && bitOffset + itemBits <= MAX_REPORT_BITS) {
          for (size_t i = 0; i < formatCount; i++) {
            if (formats[i].reportId == globals.reportId) {
              format = &formats[i];
            }
          }
          if (format == NULL && formatCount < maxFormats) {
            format = &formats[formatCount++];
            memset(format, 0, sizeof(*format));
            format->reportId = globals.reportId;
          }
        }
        if (format != NULL && format->fieldCount < FORMAT_MAX_FIELDS) {
          UsageField &field = format->fields[format->fieldCount++];
          field.bitOffset = bitOffset;
          field.bitSize = globals.reportSize;
          field.count = globals.reportCount;
          field.array = (data & 0x02) == 0;
          field.logicalMinimum = signExtend(globals.logicalMinimum, 8 * globals.logicalMinimumSize);
          // Unsigned unless the minimum is negative, so that a maximum such as 255 written in one byte is not -1
          field.logicalMaximum = field.logicalMinimum < 0
                                     ? signExtend(globals.logicalMaximum, 8 * globals.logicalMaximumSize)
                                     : static_cast<int32_t>(globals.logicalMaximum);
          field.usageMinimum = usageRange ? fullUsage(usageMinimum) & 0xFFFF : 0;
          field.usageMaximum = usageRange ? fullUsage(usageMaximum) & 0xFFFF : 0;
          field.usageCount = usageRange ? 0 : usageCount;
          field.usagesCut = !usageRange && usagesCut;
          for (size_t i = 0; i < field.usageCount; i++) {
            field.usages[i] = fullUsage(usages[i]) & 0xFFFF;
          }
        }
      }
    }
    // Local items apply only to the main item that follows them
    usageCount = 0;
    usagesCut = false;
    usageMinimum = {};
    usageMaximum = {};
    usageRange = false;
  }

  for (size_t i = 0; i < formatCount; i++) {
    for (size_t report = 0; report < reportIdCount; report++) {
      if (reportIds[report] == formats[i].reportId) {
        formats[i].bitLength = std::min(reportBits[report], MAX_REPORT_BITS);
      }
    }
  }
  return formatCount;
}

// The usage of an element of a field: the key or lamp of the element at the index of a variable field, or the key or
// lamp at the index held by an element of an array (0 if there is none)
uint16_t fieldUsage(const UsageField &field, uint32_t index) {
  if (field.usageCount == 0) {
    return field.usageMinimum <= field.usageMaximum && index <= static_cast<uint32_t>(field.usageMaximum -
                                                                                      field.usageMinimum)
               ? field.usageMinimum + index
               : 0;
  }
  if (index < field.usageCount) {
    return field.usages[index];
  }
  // The last usage also applies to the remaining elements (HID 1.11, 6.2.2.8), unless more were listed than kept
  return field.array || field.usagesCut ? 0 : field.usages[field.usageCount - 1];
}

// The index of the usage among the usages of a field (false if the field has no such usage)
bool fieldUsageIndex(const UsageField &field, uint16_t usage, uint32_t &index) {
  if (field.usageCount == 0) {
    if (usage < field.usageMinimum || usage > field.usageMaximum) {
      return false;
    }
    index = usage - field.usageMinimum;
    return true;
  }
  for (uint8_t i = 0; i < field.usageCount; i++) {
    if (field.usages[i] == usage) {
      index = i;
      return true;
    }
  }
  return false;
}

// Whether the length of a report is that of its layout in the Report Map
bool hasReportLength(const UsageFormat &format, size_t length) {
  return length == static_cast<size_t>((format.bitLength + 7) / 8);
}
