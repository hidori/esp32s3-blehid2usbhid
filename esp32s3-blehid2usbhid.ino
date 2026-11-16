// BLE HID to USB HID bridge

#include <Adafruit_TinyUSB.h>
#include <EspBle.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <nvs_flash.h>

// Interval of loop()
const uint32_t LOOP_INTERVAL_MS = 1;

// Settings HID protocol (see SPEC.md)
const uint8_t SETTINGS_REPORT_ID = 2;
const uint8_t SETTINGS_REPORT_SIZE = 63;
const uint8_t COMMAND_RESTART = 0x01;
const uint8_t COMMAND_START_SCAN = 0x04;
const uint8_t COMMAND_STOP_SCAN = 0x05;
const uint8_t COMMAND_GET_SCAN_RESULT = 0x06;
const uint8_t COMMAND_START_PAIRING = 0x07;
const uint8_t COMMAND_CANCEL_PAIRING = 0x08;
const uint8_t COMMAND_GET_PAIRING_STATE = 0x09;
const uint8_t RESULT_SUCCESS = 0x00;
const uint8_t RESULT_UNKNOWN_COMMAND = 0x01;
const uint8_t RESULT_INVALID_ARGUMENT = 0x02;
const uint8_t RESULT_BLE_FAILED = 0x04;

// Search for BLE keyboards and the scan result data (see SPEC.md)
const uint8_t SCAN_MAX_DEVICES = 16;
const size_t SCAN_NAME_MAX_SIZE = 29;  // Longest name an advertising payload (31 bytes) can carry
const size_t SCAN_RESULT_HEADER_SIZE = 11;
static_assert(SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "a scan result must fit in a response");
const uint16_t APPEARANCE_GENERIC_HID = 0x03C0;
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;
const char UUID_HID_SERVICE[] = "1812";

// Pairing state data (see SPEC.md)
const size_t PAIRING_STATE_SIZE = 5;

// Most bonds (pairing information) the NimBLE host keeps in the NVS
const size_t BOND_CAPACITY = CONFIG_BT_NIMBLE_MAX_BONDS;

// NVS (the ID marks that the NVS has been initialized by this firmware)
const char NVS_NAMESPACE[] = "settings";
const char NVS_KEY_ID[] = "id";
const uint32_t NVS_ID = 0xB1E2B5B0;

// Keyboard (boot protocol, no report ID so that boot and report protocol share the format)
uint8_t const descKeyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Consumer control (report ID 1) and vendor-defined settings collection (input and output reports)
uint8_t const descSettings[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_GENERIC_INOUT(SETTINGS_REPORT_SIZE, HID_REPORT_ID(SETTINGS_REPORT_ID)),
};

Adafruit_USBD_HID keyboard;
Adafruit_USBD_HID settings;

EspBle ble;

// USB product name with the last 2 bytes of the factory MAC address as the device ID
static char productName[40];

// Lamp state (Caps Lock etc.) received from the host
static uint8_t ledState;

// Requests from the settings page, passed from the USB task to loop()
static QueueHandle_t settingsRequests;

// BLE keyboards found by the search
struct ScannedDevice {
  uint8_t addressType;  // As in SPEC.md
  uint8_t address[6];   // Little endian
  int8_t rssi;
  uint8_t nameSize;
  char name[SCAN_NAME_MAX_SIZE];  // UTF-8, not terminated
};
static ScannedDevice scannedDevices[SCAN_MAX_DEVICES];
static uint8_t scannedCount;

// Pairing with a BLE keyboard (the values are those of SPEC.md)
enum PairingState : uint8_t {
  PAIRING_IDLE = 0,            // Not pairing
  PAIRING_CONNECTING = 1,      // Connecting
  PAIRING_PAIRING = 2,         // Pairing (also while the registration is replaced after the pairing)
  PAIRING_PASSKEY = 3,         // Waiting for the passkey to be typed on the keyboard
  PAIRING_REGISTERED = 4,      // Registered
  PAIRING_CONNECT_FAILED = 5,  // Failed to connect
  PAIRING_PAIR_FAILED = 6,     // Failed to pair
  PAIRING_SAVE_FAILED = 7,     // Failed to save the registration
};
static PairingState pairingState = PAIRING_IDLE;
static uint32_t pairingPasskey;                       // Valid in PAIRING_PASSKEY
static EspBleConnectionId pairingConnectionId;        // 0 while not connected
static bool pairingCancelRequested;                   // Canceled; ends when the connection is closed
static bool pairingBonded;                            // Made a bond; registered when the connection is closed
static EspBleBond bondsBeforePairing[BOND_CAPACITY];  // Bonds of the other devices when the pairing started
static size_t bondsBeforePairingCount;

// Output report from the host (Caps Lock etc.)
void keyboardSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
    ledState = buffer[0];
  }
}

// Output report from the settings page (runs in the USB task, so only queue the request here)
void settingsSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type != HID_REPORT_TYPE_OUTPUT || reportId != SETTINGS_REPORT_ID) {
    return;
  }
  uint8_t request[SETTINGS_REPORT_SIZE] = {};
  memcpy(request, buffer, size < sizeof(request) ? size : sizeof(request));
  xQueueSend(settingsRequests, request, 0);
}

// Make the NVS hold only the data of this firmware: erase it on the first run (when the ID is missing)
static bool initNvs() {
  Preferences prefs;
  uint32_t id = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    id = prefs.getUInt(NVS_KEY_ID, 0);
    prefs.end();
  }
  if (id == NVS_ID) {
    return true;
  }
  if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) {
    return false;
  }
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  bool written = prefs.putUInt(NVS_KEY_ID, NVS_ID) == sizeof(uint32_t);
  prefs.end();
  return written;
}

// Wait until the HID interface can send a report (gives up after about 100 ms)
static bool waitReady(Adafruit_USBD_HID &hid) {
  for (int i = 0; i < 100; i++) {
    if (hid.ready()) {
      return true;
    }
    delay(1);
  }
  return false;
}

// Parse an address written as "aa:bb:cc:dd:ee:ff" (most significant byte first) into little endian
static bool parseAddress(const String &text, uint8_t *address) {
  return sscanf(text.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &address[5], &address[4], &address[3],
                &address[2], &address[1], &address[0]) == 6;
}

// Format a little-endian address as "aa:bb:cc:dd:ee:ff" (most significant byte first), as EspBle does
static String formatAddress(uint8_t const *address) {
  char text[18];
  snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[5], address[4], address[3], address[2],
           address[1], address[0]);
  return String(text);
}

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
static ScannedDevice *findScannedDevice(uint8_t addressType, uint8_t const *address) {
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

// Clear the scan result and start the search (restarts it if it is running)
static bool startScan() {
  scannedCount = 0;
  EspBleScanConfig config;
  // Also receive the scan responses, which often carry the name
  config.active = true;
  // Receive every advertisement: the controller drops repeats by address alone, so without this a device
  // first seen before it advertises as a keyboard (e.g. before entering pairing mode) would never be listed
  config.wantDuplicates = true;
  return ble.scanner().start(config);
}

// Whether the pairing is in progress (see SPEC.md)
static bool isPairing() {
  return pairingState == PAIRING_CONNECTING || pairingState == PAIRING_PAIRING || pairingState == PAIRING_PASSKEY;
}

// Whether the two bonds are of the same device
static bool isSameBond(const EspBleBond &a, const EspBleBond &b) {
  return a.peerAddressType == b.peerAddressType && a.peerAddress.equalsIgnoreCase(b.peerAddress);
}

// Read all the bonds (false if they cannot be read)
static bool readBonds(EspBleBond *bonds, size_t &count) {
  count = min(ble.bondCount(), BOND_CAPACITY);
  for (size_t i = 0; i < count; i++) {
    if (!ble.bond(i, bonds[i])) {
      return false;
    }
  }
  return true;
}

// Whether the bond was there when the pairing started
static bool isBondBeforePairing(const EspBleBond &bond) {
  for (size_t i = 0; i < bondsBeforePairingCount; i++) {
    if (isSameBond(bondsBeforePairing[i], bond)) {
      return true;
    }
  }
  return false;
}

// Leave only the bond made by the pairing, so that it replaces the registration (needs no connection to delete a bond)
static bool replaceRegistration() {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return false;
  }
  bool made = false;
  for (size_t i = 0; i < count; i++) {
    made = made || !isBondBeforePairing(bonds[i]);
  }
  if (!made) {
    // The pairing has updated a bond with an address seen before: it can be told apart only if it is the only one
    return count == 1;
  }
  for (size_t i = 0; i < count; i++) {
    if (isBondBeforePairing(bonds[i]) && !ble.deleteBond(bonds[i])) {
      return false;
    }
  }
  return true;
}

// Start pairing with the device in the scan result (see SPEC.md)
static uint8_t startPairing(uint8_t const *args) {
  if (isPairing()) {
    return RESULT_BLE_FAILED;
  }
  const ScannedDevice *device = findScannedDevice(args[0], args + 1);
  if (device == NULL) {
    return RESULT_INVALID_ARGUMENT;
  }
  EspBleBond target;
  target.peerAddress = formatAddress(device->address);
  target.peerAddressType = static_cast<EspBleAddressType>(device->addressType);

  // Delete the bond of the device itself, or the stack would only encrypt with the old keys instead of pairing.
  // Keep the others until the pairing succeeds (see SPEC.md)
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return RESULT_BLE_FAILED;
  }
  bondsBeforePairingCount = 0;
  for (size_t i = 0; i < count; i++) {
    if (!isSameBond(bonds[i], target)) {
      bondsBeforePairing[bondsBeforePairingCount++] = bonds[i];
    } else if (!ble.deleteBond(bonds[i])) {
      return RESULT_BLE_FAILED;
    }
  }

  pairingPasskey = 0;
  pairingConnectionId = 0;
  pairingCancelRequested = false;
  pairingBonded = false;
  // connect() stops the search first, and the pairing starts as soon as connected (pairOnConnect)
  if (!ble.connect(target.peerAddress.c_str(), target.peerAddressType)) {
    return RESULT_BLE_FAILED;
  }
  pairingState = PAIRING_CONNECTING;
  return RESULT_SUCCESS;
}

// Cancel the pairing, which ends when the connection is closed (see SPEC.md)
static void cancelPairing() {
  if (!isPairing()) {
    return;
  }
  pairingCancelRequested = true;
  // A connection attempt cannot be canceled: onPairingConnected() closes the connection if it is made
  if (pairingConnectionId != 0) {
    ble.disconnect(pairingConnectionId);
  }
}

// Connected to the device to pair with
static void onPairingConnected(const EspBleConnection &connection) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingConnectionId = connection.id;
  if (pairingCancelRequested) {
    ble.disconnect(connection.id);
    return;
  }
  pairingState = PAIRING_PAIRING;
}

// Failed to connect to the device to pair with
static void onPairingConnectionFailed(const EspBleConnectionFailure &failure) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_CONNECT_FAILED;
}

// The passkey to be typed on the keyboard (Passkey Entry)
static void onPairingPasskey(const EspBlePasskeyDisplayed &event) {
  if (event.connection.id != pairingConnectionId || pairingCancelRequested) {
    return;
  }
  pairingPasskey = event.passkey;
  pairingState = PAIRING_PASSKEY;
}

// The pairing has ended; the registration is replaced after the disconnection, since a bond can be deleted only
// without a connection
static void onPairingSecurityChanged(const EspBleSecurityChanged &event) {
  if (event.connection.id != pairingConnectionId) {
    return;
  }
  pairingBonded = event.success && event.connection.bonded;
  pairingState = PAIRING_PAIRING;
  ble.disconnect(pairingConnectionId);
}

// The connection to the device to pair with is closed: the end of the pairing
static void onPairingDisconnected(const EspBleConnection &connection) {
  if (pairingConnectionId == 0 || connection.id != pairingConnectionId) {
    return;
  }
  pairingConnectionId = 0;
  if (pairingBonded) {
    pairingState = replaceRegistration() ? PAIRING_REGISTERED : PAIRING_SAVE_FAILED;
  } else {
    pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_PAIR_FAILED;
  }
}

// Send a response to the settings page
static void sendSettingsResponse(uint8_t command, uint8_t result, uint8_t const *data, size_t size) {
  uint8_t response[SETTINGS_REPORT_SIZE] = {command, result};
  if (size > 0) {
    memcpy(response + 2, data, size);
  }
  if (!waitReady(settings)) {
    return;
  }
  settings.sendReport(SETTINGS_REPORT_ID, response, sizeof(response));
}

// Send the device at the index in the scan result (see SPEC.md)
static void sendScanResult(uint8_t command, uint8_t index) {
  uint8_t data[SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE] = {};
  data[0] = ble.scanner().isScanning() ? 1 : 0;
  data[1] = scannedCount;
  size_t size = 2;
  if (index < scannedCount) {
    const ScannedDevice &device = scannedDevices[index];
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
  uint8_t data[PAIRING_STATE_SIZE] = {pairingState};
  uint32_t passkey = pairingState == PAIRING_PASSKEY ? pairingPasskey : 0;
  for (size_t i = 0; i < 4; i++) {
    data[1 + i] = (passkey >> (8 * i)) & 0xFF;
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, sizeof(data));
}

// Handle a request from the settings page
static void handleSettingsRequest(uint8_t const *request) {
  uint8_t command = request[0];
  switch (command) {
    case COMMAND_RESTART:
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      // Wait until the host has received the response
      waitReady(settings);
      ESP.restart();
      break;
    case COMMAND_START_SCAN:
      // Not while pairing (see SPEC.md)
      sendSettingsResponse(command, !isPairing() && startScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_STOP_SCAN:
      sendSettingsResponse(command, ble.scanner().stop() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_GET_SCAN_RESULT:
      sendScanResult(command, request[1]);
      break;
    case COMMAND_START_PAIRING:
      sendSettingsResponse(command, startPairing(request + 1), NULL, 0);
      break;
    case COMMAND_CANCEL_PAIRING:
      cancelPairing();
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      break;
    case COMMAND_GET_PAIRING_STATE:
      sendPairingState(command);
      break;
    default:
      sendSettingsResponse(command, RESULT_UNKNOWN_COMMAND, NULL, 0);
      break;
  }
}

void setup() {
  // Before anything else uses the NVS (the BLE bonds are kept there)
  initNvs();

  settingsRequests = xQueueCreate(4, SETTINGS_REPORT_SIZE);

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(productName, sizeof(productName), "BLE HID to USB HID Bridge (%02X%02X)", mac[4], mac[5]);

  // PID pre-allocated by Espressif for TinyUSB: 0x4000 | CDC (bit 0) | HID (bit 2).
  // Change it when the USB classes change.
  TinyUSBDevice.setID(0x303A, 0x4005);
  TinyUSBDevice.setProductDescriptor(productName);

  keyboard.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  keyboard.setReportDescriptor(descKeyboard, sizeof(descKeyboard));
  keyboard.setStringDescriptor(productName);
  keyboard.setReportCallback(NULL, keyboardSetReport);
  keyboard.begin();

  settings.setReportDescriptor(descSettings, sizeof(descSettings));
  settings.setStringDescriptor(productName);
  settings.setReportCallback(NULL, settingsSetReport);
  settings.begin();

  // USB has already started before setup() (CDC on boot), so re-enumerate to apply the settings above
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  // Pair as the side that only displays the passkey, requesting MITM protection: a keyboard that can type gets
  // Passkey Entry (the passkey is shown on the settings page), and the others get Just Works
  EspBleConfig config;
  config.security.enabled = true;
  config.security.bonding = true;
  config.security.mitm = true;
  config.security.ioCapability = EspBleSecurityIoCapability::DisplayOnly;
  // The scan results and the events of the connections are delivered from ble.update() in loop()
  ble.begin(config);
  ble.scanner().onResult(onScanResult);
  ble.onConnected(onPairingConnected);
  ble.onConnectionFailed(onPairingConnectionFailed);
  ble.onPasskeyDisplayed(onPairingPasskey);
  ble.onSecurityChanged(onPairingSecurityChanged);
  ble.onDisconnected(onPairingDisconnected);
}

void loop() {
  uint8_t request[SETTINGS_REPORT_SIZE];
  if (xQueueReceive(settingsRequests, request, 0) == pdTRUE) {
    handleSettingsRequest(request);
  }

  ble.update();

  delay(LOOP_INTERVAL_MS);
}
