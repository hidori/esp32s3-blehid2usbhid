// Layout of the keys and the lamps in the reports of a BLE keyboard, read from its Report Map (HID 1.11, 6.2.2). Uses
// neither Arduino nor BLE, so that it can be tested on a PC

#pragma once

#include <stddef.h>
#include <stdint.h>

// Usage pages of the keys and the lamps: Keyboard/Keypad, LED (lamps) and Consumer (media keys)
const uint16_t USAGE_PAGE_KEYBOARD = 0x07;
const uint16_t USAGE_PAGE_LED = 0x08;
const uint16_t USAGE_PAGE_CONSUMER = 0x0C;

// Application collections with the keys and the lamps, as a usage with its page in the upper half: a keyboard (Generic
// Desktop page, Keyboard) and a consumer control (Consumer page, Consumer Control)
const uint32_t COLLECTION_KEYBOARD = 0x00010006;
const uint32_t COLLECTION_CONSUMER_CONTROL = 0x000C0001;

// Main items of a Report Map whose fields are read: those of the input reports or of the output reports
const uint8_t ITEM_INPUT = 8;
const uint8_t ITEM_OUTPUT = 9;

// Size of a layout
const size_t FIELD_MAX_USAGES = 16;  // Usages listed one by one for a field
const size_t FORMAT_MAX_FIELDS = 4;  // Fields with keys or lamps in a report

// Keys or lamps in a field of a report: each element holds the index of a pressed key or a lit lamp (array), or the
// state of one key or lamp (variable)
struct UsageField {
  uint16_t bitOffset;      // Of the first element, in the report without the report ID
  uint8_t bitSize;         // Of an element (1 to 32)
  uint16_t count;          // Elements
  bool array;
  int32_t logicalMinimum;  // Array: the value of the first usage
  int32_t logicalMaximum;
  uint16_t usageMinimum;   // Usages on the page of the field: this range, or the list when usageCount is not 0
  uint16_t usageMaximum;
  uint8_t usageCount;
  bool usagesCut;          // More usages are listed than kept: the elements past those kept have no usage
  uint16_t usages[FIELD_MAX_USAGES];
};

// Layout of the keys in a keyboard input report or a media key report, or of the lamps in a lamp output report
struct UsageFormat {
  uint8_t reportId;    // 0 when the Report Map has no report IDs
  uint16_t bitLength;  // Of the whole report, without the report ID
  uint8_t fieldCount;
  UsageField fields[FORMAT_MAX_FIELDS];
};

// Value of a field of a HID report, least significant bit first
uint32_t readReportBits(uint8_t const *data, uint32_t bitOffset, uint8_t bitSize);

// Value of a two's complement number of the size in bits
int32_t signExtend(uint32_t value, uint8_t bits);

// Write a field of a HID report, least significant bit first
void writeReportBits(uint8_t *data, uint32_t bitOffset, uint8_t bitSize, uint32_t value);

// Read the layout of the reports of the main item (input or output) from a Report Map. Returns the number of layouts
size_t parseUsageFormats(uint8_t const *map, size_t size, uint32_t collection, uint16_t usagePage, uint8_t item,
                         UsageFormat *formats, size_t maxFormats);

// The usage of an element of a field (0 if there is none)
uint16_t fieldUsage(const UsageField &field, uint32_t index);

// The index of the usage among the usages of a field (false if the field has no such usage)
bool fieldUsageIndex(const UsageField &field, uint16_t usage, uint32_t &index);

// Whether the length of a report is that of its layout in the Report Map
bool hasReportLength(const UsageFormat &format, size_t length);
