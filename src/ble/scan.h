// Search for BLE keyboards (see SPEC.md)

#pragma once

#include <EspBle.h>

// Longest name of a device: what an advertising payload (31 bytes) can carry
const size_t SCAN_NAME_MAX_SIZE = 29;

// BLE keyboard found by the search
struct ScannedDevice {
  uint8_t addressType;  // As in SPEC.md
  uint8_t address[6];   // Little endian
  int8_t rssi;
  uint8_t nameSize;
  char name[SCAN_NAME_MAX_SIZE];  // UTF-8, not terminated
};

// Receive the advertisements found by the search
void setupScan();

// Clear the scan result and start the search (restarts it if it is running). During an attempt to connect to the
// registered keyboard, it starts when the attempt ends
bool startScan();

// Stop the search, including a requested one
bool stopScan();

// Whether the search is running or requested
bool isScanning();

// Drop the search requested during an attempt to connect
void cancelScanRequest();

// Start the search requested during an attempt to connect, once the attempt has ended (called in loop())
void startRequestedScan();

// The devices in the scan result
uint8_t getScannedCount();
const ScannedDevice &getScannedDevice(uint8_t index);

// The device with the address in the scan result (NULL if it is not there)
ScannedDevice *findScannedDevice(uint8_t addressType, uint8_t const *address);
