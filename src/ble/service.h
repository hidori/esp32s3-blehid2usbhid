// HID service of a keyboard (HID over GATT), prepared one GATT operation at a time after the encryption (EspBle queues
// at most 8 operations): discovered with EspBle on the first connection, and checked with NimBLE directly when
// remembered, since any GATT operation of EspBle discovers the whole database first. The keyboard input reports are
// prepared first, and the media key reports and the lamp output reports after the relay has started (see SPEC.md)

#pragma once

#include <EspBle.h>

#include "../hid/hid.h"
#include "gatt.h"

// The HID service, whose UUID the search also looks for
const char UUID_HID_SERVICE[] = "1812";

// Reports of the HID service that are looked at, and the longest attribute value (Bluetooth Core Specification)
const size_t HID_MAX_REPORTS = 16;
const size_t REPORT_MAP_MAX_SIZE = 512;

// Layouts of the keyboard input reports, the media key reports and the lamp output reports of the BLE keyboard, read
// from its Report Map
const size_t KEYBOARD_MAX_FORMATS = 4;  // Keyboard input reports (report IDs)
const size_t CONSUMER_MAX_FORMATS = 4;  // Media key reports (report IDs)
const size_t LAMP_MAX_FORMATS = 4;      // Lamp output reports (report IDs)

// Step of the preparation
enum HidStep : uint8_t {
  HID_STEP_NONE = 0,           // Not preparing
  HID_STEP_DISCOVER,           // Discovering the services (EspBle)
  HID_STEP_READ_REPORT_MAP,    // Reading the Report Map (EspBle)
  HID_STEP_READ_REFERENCE,     // Reading the Report Reference of each Report (EspBle)
  HID_STEP_CHECK_DESCRIPTORS,  // Checking the descriptors at the remembered handles (NimBLE)
  HID_STEP_CHECK_REFERENCE,    // Checking the Report Reference at the remembered handle (NimBLE)
  HID_STEP_SUBSCRIBE,          // Subscribing to each input report (NimBLE)
};

// A Report of the HID service
struct HidReport {
  uint16_t handle;                 // Value handle of the Report characteristic
  uint16_t referenceHandle;        // Handle of its Report Reference descriptor (0 if it has none)
  uint16_t cccdHandle;             // Handle of its Client Characteristic Configuration descriptor (0 if it has none)
  bool notifiable;
  bool writable;                   // With Write Without Response
  uint8_t id;                      // Report ID and type, from the Report Reference
  uint8_t type;
  // Layouts of a keyboard input report and of a media key report to subscribe to, and of a lamp output report to
  // write to (NULL for the others)
  const UsageFormat *keyboard;
  const UsageFormat *consumer;
  const UsageFormat *lamp;
  bool ready;                      // Prepared: its notifications are relayed, or the lamps are written to it
  // Media keys pressed in its last notification
  PressedConsumerKeys consumerPressed;
};

// The preparation, shared with the connection, the pairing and the relay in src/ble/
extern HidStep hidStep;
extern EspBleConnectionId hidConnectionId;  // The connection whose HID service is prepared (0 if none)
extern bool hidForPairing;                  // Prepared to remember it while pairing, not to relay
extern bool hidCheckReports;                // The Reports are the remembered ones, checked before subscribing
extern bool hidLatePhase;                   // Preparing the media key and lamp reports, after the relay has started
extern HidReport hidReports[HID_MAX_REPORTS];
extern size_t hidReportCount;

// The HID service of a keyboard, remembered in the NVS so that the connections need no discovery (see SPEC.md): the
// handles of the keyboard input reports, the media key reports and the lamp output reports, and the Report Map that
// tells their layout
const uint8_t HID_CACHE_VERSION = 3;  // Layout of the remembered HID service; change it when the layout changes
const size_t HID_CACHE_MAX_REPORTS = KEYBOARD_MAX_FORMATS + CONSUMER_MAX_FORMATS + LAMP_MAX_FORMATS;
struct CachedReport {
  uint16_t valueHandle;
  uint16_t cccdHandle;  // 0 for an output report
  uint16_t referenceHandle;
  uint8_t id;
  uint8_t type;
};
struct HidCache {
  uint8_t version;      // HID_CACHE_VERSION (0 if there is none)
  uint8_t addressType;  // Of the keyboard, as in SPEC.md
  uint8_t address[6];   // Little endian
  uint8_t reportCount;
  CachedReport reports[HID_CACHE_MAX_REPORTS];
  uint16_t reportMapSize;
  uint8_t reportMap[REPORT_MAP_MAX_SIZE];
};

// Receive the results of the GATT operations made with EspBle
void setupService();

// Discover the HID service with EspBle (the first step without a remembered HID service)
void startHidDiscovery();

// What to remember of the HID service just discovered (false if it cannot be kept)
bool buildHidCache(HidCache &cache);

// Take the remembered HID service as the one to prepare (false if its Report Map has no layout for a report)
bool useHidCache();

// The remembered HID service is not that of the keyboard: discover it again, or once relaying, forget it and close the
// connection
void rediscoverHidService();

// Prepare the Reports of the current phase: check them first if they are the remembered ones, then subscribe
void prepareReports();

// Subscribe to the Reports of the current phase
void subscribeReports();

// A result of a NimBLE GATT operation of the preparation
void onNimbleResult(const NimbleEvent &event);
