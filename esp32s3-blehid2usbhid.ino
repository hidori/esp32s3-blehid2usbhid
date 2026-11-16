// BLE HID to USB HID bridge

#include <Adafruit_TinyUSB.h>
#include <EspBle.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <nvs_flash.h>

// Main button (LOW while pressed)
const uint8_t PIN_BUTTON = 41;

// Time for which the button must stay in a new state before the state is accepted (debounce).
// Most switches bounce well under 10 ms, so 20 ms is conservative (Jack G. Ganssle, "A Guide to Debouncing")
const uint32_t BUTTON_DEBOUNCE_MS = 20;

// Interval of loop(), which is also the resolution of the times in a macro
const uint32_t LOOP_INTERVAL_MS = 1;

// Settings HID protocol (see SPEC.md)
const uint8_t SETTINGS_REPORT_ID = 2;
const uint8_t SETTINGS_REPORT_SIZE = 63;
const uint8_t COMMAND_RESTART = 0x01;
const uint8_t COMMAND_GET_MACRO = 0x02;
const uint8_t COMMAND_SET_MACRO = 0x03;
const uint8_t COMMAND_START_SCAN = 0x04;
const uint8_t COMMAND_STOP_SCAN = 0x05;
const uint8_t COMMAND_GET_SCAN_RESULT = 0x06;
const uint8_t RESULT_SUCCESS = 0x00;
const uint8_t RESULT_UNKNOWN_COMMAND = 0x01;
const uint8_t RESULT_INVALID_ARGUMENT = 0x02;
const uint8_t RESULT_SAVE_FAILED = 0x03;
const uint8_t RESULT_BLE_FAILED = 0x04;

// Macro data (see SPEC.md)
const uint8_t MACRO_MAX_KEYS = 10;
const uint16_t MACRO_MAX_TIME_MS = 5000;
const uint16_t MACRO_DEFAULT_GAP_MS = 100;
const size_t MACRO_HEADER_SIZE = 3;
const size_t MACRO_ENTRY_SIZE = 4;
const size_t MACRO_MAX_SIZE = MACRO_HEADER_SIZE + MACRO_ENTRY_SIZE * MACRO_MAX_KEYS;
static_assert(MACRO_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "the macro must fit in a response");

// Search for BLE keyboards and the scan result data (see SPEC.md)
const uint8_t SCAN_MAX_DEVICES = 16;
const size_t SCAN_NAME_MAX_SIZE = 29;  // Longest name an advertising payload (31 bytes) can carry
const size_t SCAN_RESULT_HEADER_SIZE = 11;
static_assert(SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "a scan result must fit in a response");
const uint16_t APPEARANCE_GENERIC_HID = 0x03C0;
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;
const char UUID_HID_SERVICE[] = "1812";

// NVS (the ID marks that the NVS has been initialized by this firmware)
const char NVS_NAMESPACE[] = "settings";
const char NVS_KEY_ID[] = "id";
const char NVS_KEY_MACRO[] = "macro";
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

// Current macro in the format of SPEC.md
static uint8_t macro[MACRO_MAX_SIZE];
static size_t macroSize;

// Sending of the macro, which proceeds a step at a time in loop() so that the settings page is answered meanwhile
enum MacroSendState : uint8_t {
  MACRO_SEND_IDLE,   // Not sending
  MACRO_SEND_PRESS,  // Pressing the key (waiting until the report can be sent)
  MACRO_SEND_HOLD,   // Holding the key until its press time passes
  MACRO_SEND_GAP,    // Waiting for the time between the keys
};
static MacroSendState macroSendState = MACRO_SEND_IDLE;
static uint8_t sendingMacro[MACRO_MAX_SIZE];  // Copy of the macro when the sending started
static uint8_t sendingIndex;                  // Index of the key being sent
static uint32_t sendingSinceMs;               // When the current step started

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

// Read a little-endian 16-bit value
static uint16_t readU16(uint8_t const *p) {
  return p[0] | (p[1] << 8);
}

// Whether the key is a usage ID allowed in a macro (see SPEC.md)
static bool isValidKey(uint8_t key) {
  return (key >= 0x04 && key <= 0xA4) || (key >= 0xB0 && key <= 0xDD);
}

// Whether the data is a macro in the format of SPEC.md
static bool isValidMacro(uint8_t const *data, size_t size) {
  if (size < MACRO_HEADER_SIZE) {
    return false;
  }
  uint8_t count = data[2];
  if (count > MACRO_MAX_KEYS || size != MACRO_HEADER_SIZE + MACRO_ENTRY_SIZE * count) {
    return false;
  }
  if (readU16(data) > MACRO_MAX_TIME_MS) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    uint8_t const *entry = data + MACRO_HEADER_SIZE + MACRO_ENTRY_SIZE * i;
    if (!isValidKey(entry[1]) || readU16(entry + 2) > MACRO_MAX_TIME_MS) {
      return false;
    }
  }
  return true;
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

// Load the macro from the NVS (no macro registered if it is missing or broken)
static void loadMacro(bool nvsReady) {
  macro[0] = MACRO_DEFAULT_GAP_MS & 0xFF;
  macro[1] = MACRO_DEFAULT_GAP_MS >> 8;
  macro[2] = 0;
  macroSize = MACRO_HEADER_SIZE;
  if (!nvsReady) {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return;
  }
  uint8_t data[MACRO_MAX_SIZE];
  size_t size = prefs.getBytes(NVS_KEY_MACRO, data, sizeof(data));
  prefs.end();
  if (isValidMacro(data, size)) {
    memcpy(macro, data, size);
    macroSize = size;
  }
}

// Save the macro to the NVS and use it
static bool saveMacro(uint8_t const *data, size_t size) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  bool saved = prefs.putBytes(NVS_KEY_MACRO, data, size) == size;
  prefs.end();
  if (saved) {
    memcpy(macro, data, size);
    macroSize = size;
  }
  return saved;
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

// Whether the main button has just been pressed (debounced, true once per press)
static bool buttonPressed() {
  static int rawState = HIGH;
  static int stableState = HIGH;
  static uint32_t rawChangedMs = 0;
  int state = digitalRead(PIN_BUTTON);
  uint32_t now = millis();
  if (state != rawState) {
    rawState = state;
    rawChangedMs = now;
    return false;
  }
  if (state != stableState && now - rawChangedMs >= BUTTON_DEBOUNCE_MS) {
    stableState = state;
    return stableState == LOW;
  }
  return false;
}

// Start sending the macro once (nothing if no macro is registered or the host is not ready)
static void startMacro() {
  if (macro[2] == 0 || !TinyUSBDevice.ready()) {
    return;
  }
  memcpy(sendingMacro, macro, macroSize);
  sendingIndex = 0;
  macroSendState = MACRO_SEND_PRESS;
}

// Proceed with sending the macro (see SPEC.md for the times)
static void updateMacro() {
  if (macroSendState == MACRO_SEND_IDLE) {
    return;
  }
  // Stop if the host is gone or suspended
  if (!TinyUSBDevice.ready()) {
    macroSendState = MACRO_SEND_IDLE;
    return;
  }

  uint8_t const *entry = sendingMacro + MACRO_HEADER_SIZE + MACRO_ENTRY_SIZE * sendingIndex;
  uint32_t now = millis();
  switch (macroSendState) {
    case MACRO_SEND_PRESS:
      if (keyboard.ready()) {
        uint8_t keys[6] = {entry[1], 0, 0, 0, 0, 0};
        keyboard.keyboardReport(0, entry[0], keys);
        sendingSinceMs = now;
        macroSendState = MACRO_SEND_HOLD;
      }
      break;
    case MACRO_SEND_HOLD:
      if (now - sendingSinceMs >= readU16(entry + 2) && keyboard.ready()) {
        keyboard.keyboardRelease(0);
        sendingSinceMs = now;
        sendingIndex++;
        macroSendState = sendingIndex < sendingMacro[2] ? MACRO_SEND_GAP : MACRO_SEND_IDLE;
      }
      break;
    case MACRO_SEND_GAP:
      // The time between the keys runs from the release of a key to the press of the next key
      if (now - sendingSinceMs >= readU16(sendingMacro)) {
        macroSendState = MACRO_SEND_PRESS;
      }
      break;
    default:
      break;
  }
}

// Parse an address written as "aa:bb:cc:dd:ee:ff" (most significant byte first) into little endian
static bool parseAddress(const String &text, uint8_t *address) {
  return sscanf(text.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &address[5], &address[4], &address[3],
                &address[2], &address[1], &address[0]) == 6;
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

// Add the device to the scan result, or update it if it is already there (see SPEC.md)
static void onScanResult(const EspBleScanResult &result) {
  uint8_t address[6];
  if (!parseAddress(result.address, address)) {
    return;
  }
  uint8_t addressType = static_cast<uint8_t>(result.addressType);
  ScannedDevice *device = NULL;
  for (uint8_t i = 0; i < scannedCount; i++) {
    if (scannedDevices[i].addressType == addressType && memcmp(scannedDevices[i].address, address, 6) == 0) {
      device = &scannedDevices[i];
      break;
    }
  }
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

// Set the macro from the arguments of the request
static uint8_t setMacro(uint8_t const *args) {
  uint8_t count = args[2];
  if (count > MACRO_MAX_KEYS) {
    return RESULT_INVALID_ARGUMENT;
  }
  size_t size = MACRO_HEADER_SIZE + MACRO_ENTRY_SIZE * count;
  if (!isValidMacro(args, size)) {
    return RESULT_INVALID_ARGUMENT;
  }
  return saveMacro(args, size) ? RESULT_SUCCESS : RESULT_SAVE_FAILED;
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
    case COMMAND_GET_MACRO:
      sendSettingsResponse(command, RESULT_SUCCESS, macro, macroSize);
      break;
    case COMMAND_SET_MACRO:
      sendSettingsResponse(command, setMacro(request + 1), NULL, 0);
      break;
    case COMMAND_START_SCAN:
      sendSettingsResponse(command, startScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_STOP_SCAN:
      sendSettingsResponse(command, ble.scanner().stop() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_GET_SCAN_RESULT:
      sendScanResult(command, request[1]);
      break;
    default:
      sendSettingsResponse(command, RESULT_UNKNOWN_COMMAND, NULL, 0);
      break;
  }
}

void setup() {
  // Before anything else uses the NVS
  bool nvsReady = initNvs();
  loadMacro(nvsReady);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

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

  // The scan results are delivered from ble.update() in loop()
  ble.begin();
  ble.scanner().onResult(onScanResult);
}

void loop() {
  uint8_t request[SETTINGS_REPORT_SIZE];
  if (xQueueReceive(settingsRequests, request, 0) == pdTRUE) {
    handleSettingsRequest(request);
  }

  // Send the macro once each time the button is pressed (a press while sending is ignored, see SPEC.md)
  if (buttonPressed() && macroSendState == MACRO_SEND_IDLE) {
    startMacro();
  }
  updateMacro();

  ble.update();

  delay(LOOP_INTERVAL_MS);
}
