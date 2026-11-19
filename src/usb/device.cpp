// USB device (see device.h)

#include "device.h"

#include <Adafruit_TinyUSB.h>
#include <esp_mac.h>

// Settings HID protocol: the report ID of the requests and the responses (see SPEC.md)
static const uint8_t SETTINGS_REPORT_ID = 2;

// Keyboard (boot protocol, no report ID so that boot and report protocol share the format)
static uint8_t const descKeyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Consumer control for the media keys: one usage of 0x000-0x3FF at a time, as the sample of Microsoft (see SPEC.md)
static const uint8_t CONSUMER_REPORT_ID = 1;

// Consumer control and vendor-defined settings collection (input and output reports)
static uint8_t const descSettings[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(CONSUMER_REPORT_ID)),
    TUD_HID_REPORT_DESC_GENERIC_INOUT(SETTINGS_REPORT_SIZE, HID_REPORT_ID(SETTINGS_REPORT_ID)),
};

static Adafruit_USBD_HID keyboard;
static Adafruit_USBD_HID settings;

// USB product name with the last 2 bytes of the factory MAC address as the device ID
static char productName[40];

// Lamps that the host has, received in the USB task
static volatile uint8_t receivedLamps;

// Requests from the settings page, passed from the USB task to loop()
static QueueHandle_t settingsRequests;

// Output report from the host: the lamps (runs in the USB task, so only keep them here; loop() writes them to the BLE
// keyboard)
static void keyboardSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
    receivedLamps = buffer[0];
  }
}

// Output report from the settings page (runs in the USB task, so only queue the request here)
static void settingsSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
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

void setupUsb() {
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

uint8_t hostLamps() {
  return receivedLamps;
}

bool receiveSettingsRequest(uint8_t *request) {
  return xQueueReceive(settingsRequests, request, 0) == pdTRUE;
}

void sendSettingsResponse(uint8_t command, uint8_t result, uint8_t const *data, size_t size) {
  uint8_t response[SETTINGS_REPORT_SIZE] = {command, result};
  if (size > 0) {
    memcpy(response + 2, data, size);
  }
  if (!waitReady(settings)) {
    return;
  }
  settings.sendReport(SETTINGS_REPORT_ID, response, sizeof(response));
}

bool waitSettingsResponseTaken() {
  return waitReady(settings);
}

bool isHostReady() {
  return TinyUSBDevice.ready();
}

bool sendKeyboardReport(uint8_t modifiers, uint8_t *keys) {
  return keyboard.ready() && keyboard.keyboardReport(0, modifiers, keys);
}

bool sendMediaKey(uint16_t usage) {
  return settings.ready() && settings.sendReport16(CONSUMER_REPORT_ID, usage);
}
