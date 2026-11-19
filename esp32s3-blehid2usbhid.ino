// BLE HID to USB HID bridge

#include "src/ble/ble.h"
#include "src/led/led.h"
#include "src/settings/settings.h"
#include "src/usb/usb.h"

// Interval of loop()
const uint32_t LOOP_INTERVAL_MS = 1;

void setup() {
  setupLed();
  setupUsb();
  setupBle();
}

void loop() {
  uint8_t request[SETTINGS_REPORT_SIZE];
  if (receiveSettingsRequest(request)) {
    handleSettingsRequest(request);
  }

  updateBle();
  sendReports();
  updateLed();

  delay(LOOP_INTERVAL_MS);
}
