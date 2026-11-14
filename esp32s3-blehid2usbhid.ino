// BLE HID to USB HID bridge

#include <Adafruit_TinyUSB.h>
#include <esp_mac.h>

// Main button (LOW while pressed)
const uint8_t PIN_BUTTON = 41;

// Keyboard (boot protocol, no report ID so that boot and report protocol share the format)
uint8_t const descKeyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Consumer control (report ID 1) and vendor-defined settings collection (report ID 2)
uint8_t const descSettings[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(1)),
    HID_USAGE_PAGE_N(HID_USAGE_PAGE_VENDOR, 2),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
      HID_REPORT_ID(2)
      HID_USAGE(0x02),
      HID_LOGICAL_MIN(0x00),
      HID_LOGICAL_MAX_N(0xff, 2),
      HID_REPORT_SIZE(8),
      HID_REPORT_COUNT(63),
      HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
};

Adafruit_USBD_HID keyboard;
Adafruit_USBD_HID settings;

// USB product name with the last 2 bytes of the factory MAC address as the device ID
static char productName[40];

// Lamp state (Caps Lock etc.) received from the host
static uint8_t ledState;

// Feature report buffer of the settings collection
static uint8_t feature[63];

// Output report from the host (Caps Lock etc.)
void keyboardSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
    ledState = buffer[0];
  }
}

// Feature report get (device -> host)
uint16_t settingsGetReport(uint8_t reportId, hid_report_type_t type, uint8_t *buffer, uint16_t reqlen) {
  if (type != HID_REPORT_TYPE_FEATURE || reportId != 2) {
    return 0;
  }
  uint16_t len = reqlen < sizeof(feature) ? reqlen : sizeof(feature);
  memcpy(buffer, feature, len);
  return len;
}

// Feature report set (host -> device)
void settingsSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type == HID_REPORT_TYPE_FEATURE && reportId == 2) {
    memcpy(feature, buffer, size < sizeof(feature) ? size : sizeof(feature));
  }
}

// Wait until the keyboard can send a report (gives up after about 100 ms)
static bool waitKeyboardReady() {
  for (int i = 0; i < 100; i++) {
    if (keyboard.ready()) {
      return true;
    }
    delay(1);
  }
  return false;
}

// Send a key once (press and release)
static void sendKey(uint8_t keycode) {
  uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
  if (!waitKeyboardReady()) {
    return;
  }
  keyboard.keyboardReport(0, 0, keys);
  if (!waitKeyboardReady()) {
    return;
  }
  keyboard.keyboardRelease(0);
}

void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);

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
  settings.setReportCallback(settingsGetReport, settingsSetReport);
  settings.begin();

  // USB has already started before setup() (CDC on boot), so re-enumerate to apply the settings above
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void loop() {
  // Send the key once when the button is pressed (the 10 ms period also filters out chattering)
  static int lastButton = HIGH;
  int button = digitalRead(PIN_BUTTON);
  if (lastButton == HIGH && button == LOW) {
    sendKey(HID_KEY_A);
  }
  lastButton = button;
  delay(10);
}
