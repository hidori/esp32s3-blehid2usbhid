// HID service of a keyboard (see service.h)

#include "service.h"

#include "ble.h"
#include "connection.h"
#include "pairing.h"
#include "storage.h"

// HID over GATT: the Report Map and the Reports of the HID service, the descriptors of a Report (Report Reference, and
// the Client Characteristic Configuration to subscribe), and the types of the reports in the Report Reference
static const char UUID_REPORT_MAP[] = "2a4b";
static const char UUID_REPORT[] = "2a4d";
static const char UUID_REPORT_REFERENCE[] = "2908";
static const char UUID_CCCD[] = "2902";
static const uint16_t UUID16_REPORT_REFERENCE = 0x2908;
static const uint16_t UUID16_CCCD = 0x2902;
static const uint8_t REPORT_TYPE_INPUT = 1;
static const uint8_t REPORT_TYPE_OUTPUT = 2;

// The preparation (see service.h)
HidStep hidStep;
EspBleConnectionId hidConnectionId;
bool hidForPairing;
bool hidCheckReports;
bool hidLatePhase;
HidReport hidReports[HID_MAX_REPORTS];
size_t hidReportCount;
static size_t hidStepIndex;                        // The Report being read, checked or subscribed to
static uint8_t hidCheckedDescriptors;              // Descriptors found as remembered for the Report being checked
static uint16_t hidReportMapHandle;
static uint8_t hidReportMap[REPORT_MAP_MAX_SIZE];  // The Report Map read, kept to be remembered
static size_t hidReportMapSize;                    // 0 if it is too long to be kept
static UsageFormat keyboardFormats[KEYBOARD_MAX_FORMATS];
static size_t keyboardFormatCount;
static UsageFormat consumerFormats[CONSUMER_MAX_FORMATS];
static size_t consumerFormatCount;
static UsageFormat lampFormats[LAMP_MAX_FORMATS];
static size_t lampFormatCount;

// Each step goes on to the next one when its GATT operation ends, so these are used before they are defined
static void subscribeNextReport();
static void checkNextCachedReport();

// Whether a UUID that EspBle reports (the 128-bit form on the Bluetooth base UUID) is the 16-bit UUID written in 4
// hex digits
static bool isUuid16(const String &uuid, const char *uuid16) {
  return uuid.equalsIgnoreCase(uuid16) ||
         uuid.equalsIgnoreCase(String("0000") + uuid16 + "-0000-1000-8000-00805f9b34fb");
}

// Read the layouts of the keyboard input reports, of the media key reports and of the lamp output reports from a Report
// Map
static void parseHidFormats(uint8_t const *map, size_t size) {
  keyboardFormatCount = parseUsageFormats(map, size, COLLECTION_KEYBOARD, USAGE_PAGE_KEYBOARD, ITEM_INPUT,
                                          keyboardFormats, KEYBOARD_MAX_FORMATS);
  consumerFormatCount = parseUsageFormats(map, size, COLLECTION_CONSUMER_CONTROL, USAGE_PAGE_CONSUMER, ITEM_INPUT,
                                          consumerFormats, CONSUMER_MAX_FORMATS);
  lampFormatCount =
      parseUsageFormats(map, size, COLLECTION_KEYBOARD, USAGE_PAGE_LED, ITEM_OUTPUT, lampFormats, LAMP_MAX_FORMATS);
}

// The layout of the report with the ID among the layouts (NULL if there is none)
static const UsageFormat *findFormat(const UsageFormat *formats, size_t count, uint8_t reportId) {
  for (size_t i = 0; i < count; i++) {
    if (formats[i].reportId == reportId) {
      return &formats[i];
    }
  }
  return NULL;
}

// Take the layouts of the Report from its type and ID: a keyboard input report or a media key report if it is an input
// report that can be subscribed to, and a lamp output report if it is an output report that can be written to
static void findReportFormats(HidReport &report) {
  bool input = report.type == REPORT_TYPE_INPUT && report.notifiable && report.cccdHandle != 0;
  bool output = report.type == REPORT_TYPE_OUTPUT && report.writable;
  report.keyboard = input ? findFormat(keyboardFormats, keyboardFormatCount, report.id) : NULL;
  report.consumer = input ? findFormat(consumerFormats, consumerFormatCount, report.id) : NULL;
  report.lamp = output ? findFormat(lampFormats, lampFormatCount, report.id) : NULL;
}

// Whether the Report is used: a keyboard input report, a media key report or a lamp output report
static bool isUsedReport(const HidReport &report) {
  return report.keyboard != NULL || report.consumer != NULL || report.lamp != NULL;
}

// The HID service cannot be prepared: close the connection. For the relay, the next attempt tries again. For the
// pairing, the device is registered without remembering its HID service, unless the HID service shows that the device
// is not a keyboard (unusable), see SPEC.md
static void failHidPreparation(bool unusable) {
  hidStep = HID_STEP_NONE;
  if (hidForPairing) {
    setPairingNotKeyboard(unusable);
  }
  ble.disconnect(hidConnectionId);
}

// Discover the HID service with EspBle (the first step without a remembered HID service)
void startHidDiscovery() {
  hidStep = HID_STEP_DISCOVER;
  hidCheckReports = false;
  hidReportMapSize = 0;
  if (!ble.discoverServices(hidConnectionId)) {
    failHidPreparation(false);
  }
}

// Find the Report Map and the Reports of the first HID service among the discovered services
static bool findHidReports() {
  EspBleConnectionId id = hidConnectionId;
  // The service starts at its handle and ends before the next service
  uint16_t first = 0;
  uint16_t next = 0;
  EspBleGattServiceInfo service;
  for (size_t i = 0; ble.discoveredService(id, i, service); i++) {
    if (isUuid16(service.serviceUuid, UUID_HID_SERVICE) && (first == 0 || service.handle < first)) {
      first = service.handle;
    }
  }
  if (first == 0) {
    return false;
  }
  for (size_t i = 0; ble.discoveredService(id, i, service); i++) {
    if (service.handle > first && (next == 0 || service.handle < next)) {
      next = service.handle;
    }
  }
  auto inService = [first, next](uint16_t handle) { return handle > first && (next == 0 || handle < next); };

  hidReportMapHandle = 0;
  hidReportCount = 0;
  EspBleGattCharacteristicInfo characteristic;
  for (size_t i = 0; ble.discoveredCharacteristic(id, i, characteristic, UUID_HID_SERVICE); i++) {
    if (!inService(characteristic.handle)) {
      continue;
    }
    if (isUuid16(characteristic.characteristicUuid, UUID_REPORT_MAP) && hidReportMapHandle == 0) {
      hidReportMapHandle = characteristic.handle;
    } else if (isUuid16(characteristic.characteristicUuid, UUID_REPORT) && hidReportCount < HID_MAX_REPORTS) {
      HidReport &report = hidReports[hidReportCount++];
      report = {};
      report.handle = characteristic.handle;
      report.notifiable = characteristic.notifiable;
      report.writable = characteristic.writableWithoutResponse;
    }
  }
  // All Reports have the same UUID, so their descriptors are matched by the handle of the Report
  EspBleGattDescriptorInfo descriptor;
  for (size_t i = 0; ble.discoveredDescriptor(id, i, descriptor, UUID_HID_SERVICE, UUID_REPORT); i++) {
    bool reference = isUuid16(descriptor.descriptorUuid, UUID_REPORT_REFERENCE);
    bool cccd = isUuid16(descriptor.descriptorUuid, UUID_CCCD);
    for (size_t r = 0; r < hidReportCount; r++) {
      if (hidReports[r].handle != descriptor.characteristicHandle) {
        continue;
      }
      if (reference) {
        hidReports[r].referenceHandle = descriptor.handle;
      } else if (cccd) {
        hidReports[r].cccdHandle = descriptor.handle;
      }
    }
  }
  return hidReportMapHandle != 0 && hidReportCount > 0;
}

// What to remember of the HID service just discovered: the keyboard input reports, the media key reports, the lamp
// output reports and the Report Map. The address is set when it is saved (false if it cannot be kept)
bool buildHidCache(HidCache &cache) {
  memset(&cache, 0, sizeof(cache));
  if (hidReportMapSize == 0) {
    return false;
  }
  for (size_t i = 0; i < hidReportCount; i++) {
    const HidReport &report = hidReports[i];
    if (!isUsedReport(report)) {
      continue;
    }
    if (cache.reportCount >= HID_CACHE_MAX_REPORTS) {
      return false;
    }
    cache.reports[cache.reportCount++] = {report.handle, report.cccdHandle, report.referenceHandle, report.id,
                                          report.type};
  }
  if (cache.reportCount == 0) {
    return false;
  }
  cache.reportMapSize = hidReportMapSize;
  memcpy(cache.reportMap, hidReportMap, hidReportMapSize);
  cache.version = HID_CACHE_VERSION;
  return true;
}

// Take the remembered HID service as the one to prepare (false if its Report Map has no layout for a report)
bool useHidCache() {
  const HidCache &hidCache = getHidCache();
  parseHidFormats(hidCache.reportMap, hidCache.reportMapSize);
  hidReportCount = 0;
  for (size_t i = 0; i < hidCache.reportCount; i++) {
    const CachedReport &cached = hidCache.reports[i];
    HidReport &report = hidReports[hidReportCount++];
    report = {};
    report.handle = cached.valueHandle;
    report.referenceHandle = cached.referenceHandle;
    report.cccdHandle = cached.cccdHandle;
    report.notifiable = cached.type == REPORT_TYPE_INPUT;
    report.writable = cached.type == REPORT_TYPE_OUTPUT;
    report.id = cached.id;
    report.type = cached.type;
    findReportFormats(report);
    if (!isUsedReport(report)) {
      return false;
    }
  }
  return hidReportCount > 0;
}

// The remembered HID service is not that of the keyboard: discover it as on the first connection, and remember it again
// when the relay starts. Once relaying, forget it and close the connection instead, so that the next connection
// discovers it without stopping the relay in the middle (see SPEC.md)
void rediscoverHidService() {
  if (hidLatePhase) {
    forgetHidCache();
    failHidPreparation(false);
    return;
  }
  keyboardUsesCache = false;
  if (!keyboardShortInterval) {
    keyboardShortInterval = askShortInterval(keyboardConnectionId, keyboardSupervisionTimeout);
  }
  startHidDiscovery();
}

// Whether the Report is prepared in the current phase: the keyboard input reports, then the media key reports and the
// lamp output reports (a report with both keys and media keys is prepared with the keyboard input reports)
static bool isPhaseReport(const HidReport &report) {
  return hidLatePhase ? report.keyboard == NULL && (report.consumer != NULL || report.lamp != NULL)
                      : report.keyboard != NULL;
}

// Skip to the next Report of the current phase
static void skipToPhaseReport() {
  while (hidStepIndex < hidReportCount && !isPhaseReport(hidReports[hidStepIndex])) {
    hidStepIndex++;
  }
}

// Prepare the Reports of the current phase: check them first if they are the remembered ones, then subscribe
void prepareReports() {
  hidStepIndex = 0;
  if (hidCheckReports) {
    checkNextCachedReport();
  } else {
    subscribeReports();
  }
}

// Subscribe to the Reports of the current phase
void subscribeReports() {
  hidStep = HID_STEP_SUBSCRIBE;
  hidStepIndex = 0;
  subscribeNextReport();
}

// Subscribe to the next input report of the current phase by writing its Client Characteristic Configuration (an output
// report is ready without it). When all the keyboard input reports are subscribed, the relay starts and the media key
// reports and the lamp output reports are prepared (see SPEC.md)
static void subscribeNextReport() {
  skipToPhaseReport();
  while (hidStepIndex < hidReportCount && hidReports[hidStepIndex].type != REPORT_TYPE_INPUT) {
    hidReports[hidStepIndex].ready = true;
    hidStepIndex++;
    skipToPhaseReport();
  }
  if (hidStepIndex >= hidReportCount) {
    if (hidLatePhase) {
      hidStep = HID_STEP_NONE;
      return;
    }
    startRelaying();
    hidLatePhase = true;
    prepareReports();
    return;
  }
  static const uint8_t NOTIFICATIONS_ENABLED[] = {0x01, 0x00};
  if (ble_gattc_write_flat(keyboardConnectionHandle, hidReports[hidStepIndex].cccdHandle, NOTIFICATIONS_ENABLED,
                           sizeof(NOTIFICATIONS_ENABLED), onNimbleAttribute,
                           nextOperationTag()) != 0) {
    failHidPreparation(false);
  }
}

// Check the next remembered Report of the current phase: the descriptors at its handles, then its Report Reference.
// Subscribe when all are as remembered
static void checkNextCachedReport() {
  skipToPhaseReport();
  if (hidStepIndex >= hidReportCount) {
    subscribeReports();
    return;
  }
  const HidReport &report = hidReports[hidStepIndex];
  hidStep = HID_STEP_CHECK_DESCRIPTORS;
  hidCheckedDescriptors = 0;
  if (ble_gattc_disc_all_dscs(keyboardConnectionHandle, report.handle, max(report.cccdHandle, report.referenceHandle),
                              onNimbleDescriptor, nextOperationTag()) != 0) {
    rediscoverHidService();
  }
}

// Read the Report Reference of the next Report that has one and can be notified or written without response (the others
// cannot be keyboard input reports, media key reports or lamp output reports). When all are read, subscribe to the
// Reports, or end the pairing
static void readNextReportReference() {
  while (hidStepIndex < hidReportCount &&
         (hidReports[hidStepIndex].referenceHandle == 0 ||
          (!hidReports[hidStepIndex].notifiable && !hidReports[hidStepIndex].writable))) {
    hidStepIndex++;
  }
  if (hidStepIndex < hidReportCount) {
    if (!ble.readDescriptor(hidConnectionId, hidReports[hidStepIndex].referenceHandle)) {
      failHidPreparation(false);
    }
    return;
  }
  bool found = false;
  for (size_t i = 0; i < hidReportCount; i++) {
    found = found || hidReports[i].keyboard != NULL;
  }
  if (!found) {
    failHidPreparation(true);
    return;
  }
  if (hidForPairing) {
    finishPairingPreparation();
    return;
  }
  subscribeReports();
}

// A result of a NimBLE GATT operation of the preparation
void onNimbleResult(const NimbleEvent &event) {
  if (hidForPairing || hidConnectionId != keyboardConnectionId || event.connectionHandle != keyboardConnectionHandle ||
      hidStepIndex >= hidReportCount) {
    return;
  }
  HidReport &report = hidReports[hidStepIndex];
  switch (hidStep) {
    case HID_STEP_CHECK_DESCRIPTORS:
      if (event.type == NIMBLE_DESCRIPTOR) {
        if ((event.handle == report.cccdHandle && event.uuid16 == UUID16_CCCD) ||
            (event.handle == report.referenceHandle && event.uuid16 == UUID16_REPORT_REFERENCE)) {
          hidCheckedDescriptors++;
        }
        return;
      }
      // The Report Reference, and the Client Characteristic Configuration of an input report
      if (event.status != 0 || hidCheckedDescriptors != (report.cccdHandle != 0 ? 2 : 1)) {
        rediscoverHidService();
        return;
      }
      hidStep = HID_STEP_CHECK_REFERENCE;
      if (ble_gattc_read(keyboardConnectionHandle, report.referenceHandle, onNimbleAttribute,
                         nextOperationTag()) != 0) {
        rediscoverHidService();
      }
      return;
    case HID_STEP_CHECK_REFERENCE:
      if (event.status != 0 || event.length < 2 || event.data[0] != report.id || event.data[1] != report.type) {
        rediscoverHidService();
        return;
      }
      hidStepIndex++;
      checkNextCachedReport();
      return;
    case HID_STEP_SUBSCRIBE:
      if (event.status != 0) {
        failHidPreparation(false);
        return;
      }
      report.ready = true;
      hidStepIndex++;
      subscribeNextReport();
      return;
    default:
      return;
  }
}

// The services are discovered: read the Report Map of the HID service. Without the HID service, the device is not a
// keyboard
static void onServicesDiscovered(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_DISCOVER || result.connectionId != hidConnectionId) {
    return;
  }
  if (!result.success) {
    failHidPreparation(false);
    return;
  }
  if (!findHidReports()) {
    failHidPreparation(true);
    return;
  }
  hidStep = HID_STEP_READ_REPORT_MAP;
  if (!ble.readCharacteristic(hidConnectionId, hidReportMapHandle)) {
    failHidPreparation(false);
  }
}

// The Report Map is read: keep it to be remembered, find the layout of the keyboard input reports and of the media key
// reports, then read the Report References. Without a keyboard input in the Report Map, the device is not a keyboard
static void onCharacteristicRead(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_READ_REPORT_MAP || result.connectionId != hidConnectionId ||
      result.handle != hidReportMapHandle) {
    return;
  }
  if (!result.success) {
    failHidPreparation(false);
    return;
  }
  uint8_t const *map = reinterpret_cast<uint8_t const *>(result.value.c_str());
  hidReportMapSize = result.value.length() <= REPORT_MAP_MAX_SIZE ? result.value.length() : 0;
  memcpy(hidReportMap, map, hidReportMapSize);
  parseHidFormats(map, result.value.length());
  if (keyboardFormatCount == 0) {
    failHidPreparation(true);
    return;
  }
  hidStep = HID_STEP_READ_REFERENCE;
  hidStepIndex = 0;
  readNextReportReference();
}

// A Report Reference is read: the Report is a keyboard input report or a media key report if it is an input report with
// the ID of one in the Report Map and can be subscribed to, and a lamp output report if it is an output report with the
// ID of one and can be written to
static void onDescriptorRead(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_READ_REFERENCE || result.connectionId != hidConnectionId ||
      hidStepIndex >= hidReportCount || result.descriptorHandle != hidReports[hidStepIndex].referenceHandle) {
    return;
  }
  if (!result.success || result.value.length() < 2) {
    failHidPreparation(false);
    return;
  }
  HidReport &report = hidReports[hidStepIndex];
  report.id = static_cast<uint8_t>(result.value[0]);
  report.type = static_cast<uint8_t>(result.value[1]);
  findReportFormats(report);
  hidStepIndex++;
  readNextReportReference();
}

void setupService() {
  ble.onServicesDiscovered(onServicesDiscovered);
  ble.onCharacteristicRead(onCharacteristicRead);
  ble.onDescriptorRead(onDescriptorRead);
}
