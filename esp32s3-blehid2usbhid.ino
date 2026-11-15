// BLE HID to USB HID bridge

#include <Adafruit_TinyUSB.h>
#include <esp_mac.h>

// Main button (LOW while pressed)
const uint8_t PIN_BUTTON = 41;

// Settings HID protocol (see SPEC.md)
const uint8_t SETTINGS_REPORT_ID = 2;
const uint8_t SETTINGS_REPORT_SIZE = 63;
const uint8_t COMMAND_RESTART = 0x01;
const uint8_t RESULT_SUCCESS = 0x00;
const uint8_t RESULT_UNKNOWN_COMMAND = 0x01;

// Keyboard (boot protocol, no report ID so that boot and report protocol share the format)
uint8_t const descKeyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Consumer control (report ID 1) and vendor-defined settings collection (input and output reports)
uint8_t const descSettings[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_GENERIC_INOUT(SETTINGS_REPORT_SIZE, HID_REPORT_ID(SETTINGS_REPORT_ID)),
};

Adafruit_USBD_HID keyboard;
Adafruit_USBD_HID settings;

// USB product name with the last 2 bytes of the factory MAC address as the device ID
static char productName[40];

// Lamp state (Caps Lock etc.) received from the host
static uint8_t ledState;

// Requests from the settings page, passed from the USB task to loop()
static QueueHandle_t settingsRequests;

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

// Send a key once (press and release)
static void sendKey(uint8_t keycode) {
  uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
  if (!waitReady(keyboard)) {
    return;
  }
  keyboard.keyboardReport(0, 0, keys);
  if (!waitReady(keyboard)) {
    return;
  }
  keyboard.keyboardRelease(0);
}

// Send a response to the settings page
static void sendSettingsResponse(uint8_t command, uint8_t result) {
  uint8_t response[SETTINGS_REPORT_SIZE] = {command, result};
  if (!waitReady(settings)) {
    return;
  }
  settings.sendReport(SETTINGS_REPORT_ID, response, sizeof(response));
}

// Handle a request from the settings page
static void handleSettingsRequest(uint8_t const *request) {
  uint8_t command = request[0];
  switch (command) {
    case COMMAND_RESTART:
      sendSettingsResponse(command, RESULT_SUCCESS);
      // Wait until the host has received the response
      waitReady(settings);
      ESP.restart();
      break;
    default:
      sendSettingsResponse(command, RESULT_UNKNOWN_COMMAND);
      break;
  }
}

void setup() {
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
}

void loop() {
  uint8_t request[SETTINGS_REPORT_SIZE];
  if (xQueueReceive(settingsRequests, request, 0) == pdTRUE) {
    handleSettingsRequest(request);
  }

  // Send the key once when the button is pressed (polled every 10 ms, no debounce yet)
  static int lastButton = HIGH;
  int button = digitalRead(PIN_BUTTON);
  if (lastButton == HIGH && button == LOW) {
    sendKey(HID_KEY_A);
  }
  lastButton = button;
  delay(10);
}
