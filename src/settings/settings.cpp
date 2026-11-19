// Settings HID protocol (see settings.h)

#include "settings.h"

#include <Arduino.h>

#include "../ble/ble.h"
#include "../usb/usb.h"

// Commands and results of the requests (see SPEC.md)
static const uint8_t COMMAND_RESTART = 0x01;
static const uint8_t COMMAND_START_SCAN = 0x04;
static const uint8_t COMMAND_STOP_SCAN = 0x05;
static const uint8_t COMMAND_GET_SCAN_RESULT = 0x06;
static const uint8_t COMMAND_START_PAIRING = 0x07;
static const uint8_t COMMAND_CANCEL_PAIRING = 0x08;
static const uint8_t COMMAND_GET_PAIRING_STATE = 0x09;
static const uint8_t COMMAND_GET_REGISTRATION = 0x0A;
static const uint8_t COMMAND_DELETE_REGISTRATION = 0x0B;
static const uint8_t RESULT_SUCCESS = 0x00;
static const uint8_t RESULT_UNKNOWN_COMMAND = 0x01;
static const uint8_t RESULT_INVALID_ARGUMENT = 0x02;
static const uint8_t RESULT_BLE_FAILED = 0x04;

// Scan result data (see SPEC.md)
static const size_t SCAN_RESULT_HEADER_SIZE = 11;
static_assert(SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "a scan result must fit in a response");

// Pairing state data (see SPEC.md)
static const size_t PAIRING_STATE_SIZE = 5;

// Registration data (see SPEC.md)
static const size_t REGISTRATION_HEADER_SIZE = 10;
static_assert(REGISTRATION_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "the registration must fit in a response");

// Send the device at the index in the scan result (see SPEC.md)
static void sendScanResult(uint8_t command, uint8_t index) {
  uint8_t data[SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE] = {};
  uint8_t count = getScannedCount();
  data[0] = isScanning() ? 1 : 0;
  data[1] = count;
  size_t size = 2;
  if (index < count) {
    const ScannedDevice &device = getScannedDevice(index);
    data[2] = device.addressType;
    memcpy(data + 3, device.address, 6);
    data[9] = static_cast<uint8_t>(device.rssi);
    data[10] = device.nameSize;
    memcpy(data + SCAN_RESULT_HEADER_SIZE, device.name, device.nameSize);
    size = SCAN_RESULT_HEADER_SIZE + device.nameSize;
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, size);
}

// Send the pairing state (see SPEC.md)
static void sendPairingState(uint8_t command) {
  PairingState state = getPairingState();
  uint8_t data[PAIRING_STATE_SIZE] = {state};
  uint32_t passkey = state == PAIRING_PASSKEY ? getPairingPasskey() : 0;
  for (size_t i = 0; i < 4; i++) {
    data[1 + i] = (passkey >> (8 * i)) & 0xFF;
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, sizeof(data));
}

// Send the registered keyboard and the state of the connection to it (see SPEC.md)
static void sendRegistration(uint8_t command) {
  uint8_t data[REGISTRATION_HEADER_SIZE + SCAN_NAME_MAX_SIZE] = {};
  size_t size = 1;
  EspBleBond registered;
  if (readRegisteredKeyboard(registered) && parseAddress(registered.peerAddress, data + 2)) {
    data[0] = 1;
    data[1] = static_cast<uint8_t>(registered.peerAddressType);
    data[8] = keyboardState();
    size = REGISTRATION_HEADER_SIZE;
    // The saved name, only if it is that of the registered keyboard
    const KeyboardName &name = getKeyboardName();
    if (name.addressType == data[1] && memcmp(name.address, data + 2, 6) == 0) {
      data[9] = name.nameSize;
      memcpy(data + REGISTRATION_HEADER_SIZE, name.name, name.nameSize);
      size += name.nameSize;
    }
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, size);
}

// Start pairing with the device in the arguments of the request (see SPEC.md)
static uint8_t startPairingRequest(uint8_t const *args) {
  switch (startPairing(args[0], args + 1)) {
    case PAIRING_STARTED:
      return RESULT_SUCCESS;
    case PAIRING_UNKNOWN_DEVICE:
      return RESULT_INVALID_ARGUMENT;
    default:
      return RESULT_BLE_FAILED;
  }
}

void handleSettingsRequest(uint8_t const *request) {
  uint8_t command = request[0];
  switch (command) {
    case COMMAND_RESTART:
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      // Wait until the host has received the response
      waitSettingsResponseTaken();
      ESP.restart();
      break;
    case COMMAND_START_SCAN:
      // Not while pairing (see SPEC.md)
      sendSettingsResponse(command, !isPairing() && startScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_STOP_SCAN:
      sendSettingsResponse(command, stopScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_GET_SCAN_RESULT:
      sendScanResult(command, request[1]);
      break;
    case COMMAND_START_PAIRING:
      sendSettingsResponse(command, startPairingRequest(request + 1), NULL, 0);
      break;
    case COMMAND_CANCEL_PAIRING:
      cancelPairing();
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      break;
    case COMMAND_GET_PAIRING_STATE:
      sendPairingState(command);
      break;
    case COMMAND_GET_REGISTRATION:
      sendRegistration(command);
      break;
    case COMMAND_DELETE_REGISTRATION:
      sendSettingsResponse(command, startRegistrationDeletion() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    default:
      sendSettingsResponse(command, RESULT_UNKNOWN_COMMAND, NULL, 0);
      break;
  }
}
