// Search for BLE keyboards (see scan.h)

#include "scan.h"

#include "address.h"
#include "ble.h"
#include "connection.h"
#include "service.h"

// Most devices in the scan result (see SPEC.md)
static const uint8_t SCAN_MAX_DEVICES = 16;

// Appearances of a keyboard (see SPEC.md)
static const uint16_t APPEARANCE_GENERIC_HID = 0x03C0;
static const uint16_t APPEARANCE_KEYBOARD = 0x03C1;

// BLE keyboards found by the search
static ScannedDevice scannedDevices[SCAN_MAX_DEVICES];
static uint8_t scannedCount;

// Search requested while an attempt to connect to the registered keyboard is in progress (started when it ends)
static bool scanRequested;

// Length of the UTF-8 text cut to at most maxSize bytes without splitting a character
static size_t truncateUtf8(const char *text, size_t size, size_t maxSize) {
  if (size <= maxSize) {
    return size;
  }
  size_t cut = maxSize;
  while (cut > 0 && (text[cut] & 0xC0) == 0x80) {
    cut--;
  }
  return cut;
}

// Whether the advertisement is from a connectable device that looks like a keyboard (see SPEC.md)
static bool isKeyboardAdvertisement(const EspBleScanResult &result) {
  if (!result.connectable) {
    return false;
  }
  if (result.appearance == APPEARANCE_KEYBOARD) {
    return true;
  }
  return result.advertisesService(UUID_HID_SERVICE) &&
         (result.appearance == 0 || result.appearance == APPEARANCE_GENERIC_HID);
}

// The device with the address in the scan result (NULL if it is not there)
ScannedDevice *findScannedDevice(uint8_t addressType, uint8_t const *address) {
  for (uint8_t i = 0; i < scannedCount; i++) {
    if (scannedDevices[i].addressType == addressType && memcmp(scannedDevices[i].address, address, 6) == 0) {
      return &scannedDevices[i];
    }
  }
  return NULL;
}

// Add the device to the scan result, or update it if it is already there (see SPEC.md)
static void onScanResult(const EspBleScanResult &result) {
  uint8_t address[6];
  if (!parseAddress(result.address, address)) {
    return;
  }
  uint8_t addressType = static_cast<uint8_t>(result.addressType);
  ScannedDevice *device = findScannedDevice(addressType, address);
  if (device == NULL) {
    if (scannedCount >= SCAN_MAX_DEVICES || !isKeyboardAdvertisement(result)) {
      return;
    }
    device = &scannedDevices[scannedCount++];
    memset(device, 0, sizeof(*device));
    device->addressType = addressType;
    memcpy(device->address, address, 6);
  }
  device->rssi = static_cast<int8_t>(result.rssi);
  if (result.hasName()) {
    const char *name = result.name.c_str();
    size_t nameSize = truncateUtf8(name, result.name.length(), SCAN_NAME_MAX_SIZE);
    // A shortened name is the beginning of the complete name (Bluetooth Core Specification), and EspBle does not
    // tell them apart: keep the complete name, e.g. from the scan response, when only the advertisement comes
    bool shortened = nameSize < device->nameSize && memcmp(device->name, name, nameSize) == 0;
    if (!shortened) {
      device->nameSize = nameSize;
      memcpy(device->name, name, nameSize);
    }
  }
}

// Start the search with the settings of SPEC.md
static bool beginScan() {
  EspBleScanConfig config;
  // Also receive the scan responses, which often carry the name
  config.active = true;
  // Receive every advertisement: the controller drops repeats by address alone, so without this a device
  // first seen before it advertises as a keyboard (e.g. before entering pairing mode) would never be listed
  config.wantDuplicates = true;
  return ble.scanner().start(config);
}

// Clear the scan result and start the search (restarts it if it is running). A search cannot start while connecting
// (BLE_HS_EBUSY), so during an attempt to connect to the registered keyboard it starts when the attempt ends
bool startScan() {
  scannedCount = 0;
  if (keyboardConnecting) {
    scanRequested = true;
    return true;
  }
  return beginScan();
}

// Stop the search, including a requested one
bool stopScan() {
  scanRequested = false;
  return ble.scanner().stop();
}

// Whether the search is running or requested
bool isScanning() {
  return scanRequested || ble.scanner().isScanning();
}

void setupScan() {
  ble.scanner().onResult(onScanResult);
}

void cancelScanRequest() {
  scanRequested = false;
}

// A search requested during an attempt to connect starts when the attempt has ended
void startRequestedScan() {
  if (scanRequested && !keyboardConnecting) {
    scanRequested = false;
    beginScan();
  }
}

uint8_t getScannedCount() {
  return scannedCount;
}

const ScannedDevice &getScannedDevice(uint8_t index) {
  return scannedDevices[index];
}
