// Relay of the input of the registered keyboard and of the lamps of the host (see relay.h)

#include "relay.h"

#include "../hid/hid.h"
#include "../usb/usb.h"
#include "connection.h"
#include "service.h"

// Longest lamp output report written: the value of a Write Without Response that fits the smallest ATT MTU (23 bytes)
static const size_t LAMP_REPORT_MAX_SIZE = 20;

// Lamps last written to the BLE keyboard on this connection (-1 if none), as hostLamps()
static int16_t writtenLamps = -1;

// Relay a media key report: send the media key chosen as the guidelines of Microsoft (see SPEC.md)
static void relayConsumerReport(HidReport &hidReport, uint8_t const *data, size_t length) {
  PressedConsumerKeys pressed;
  if (!readConsumerReport(*hidReport.consumer, data, length, pressed)) {
    return;
  }
  queueMediaKey(chooseConsumerUsage(lastQueuedMediaKey(), hidReport.consumerPressed, pressed));
  hidReport.consumerPressed = pressed;
}

// Relay a keyboard input report of the registered keyboard as a boot keyboard report, and a media key report as the
// media key (see SPEC.md). Only the connection encrypted with the bond gets ready to relay, and only the Reports
// subscribed to after their handles have been checked are relayed (a bonded keyboard may notify before that)
void relayNotification(const NimbleEvent &event) {
  if (!keyboardReady || event.connectionHandle != keyboardConnectionHandle || event.status != 0) {
    return;
  }
  for (size_t i = 0; i < hidReportCount; i++) {
    HidReport &hidReport = hidReports[i];
    if (hidReport.handle != event.handle || !hidReport.ready) {
      continue;
    }
    KeyboardReport report;
    if (hidReport.keyboard != NULL && readKeyboardReport(*hidReport.keyboard, event.data, event.length, report)) {
      queueKeyboardReport(report.modifiers, report.keys);
    }
    if (hidReport.consumer != NULL) {
      relayConsumerReport(hidReport, event.data, event.length);
    }
    return;
  }
}

// Write the lamps of the host to the lamp output reports of the BLE keyboard when they change, and once the reports are
// ready on a connection (see SPEC.md). Written without response, so that no GATT operation of the preparation waits;
// if a write cannot be queued, all are written again in the next loop()
void writeLamps() {
  uint8_t lamps = hostLamps();
  if (!keyboardReady || lamps == writtenLamps) {
    return;
  }
  bool written = false;
  for (size_t i = 0; i < hidReportCount; i++) {
    const HidReport &report = hidReports[i];
    if (report.lamp == NULL || !report.ready) {
      continue;
    }
    uint8_t data[LAMP_REPORT_MAX_SIZE];
    size_t length = makeLampReport(*report.lamp, lamps, data, sizeof(data));
    if (length == 0) {
      continue;
    }
    if (ble_gattc_write_no_rsp_flat(keyboardConnectionHandle, report.handle, data, length) != 0) {
      return;
    }
    written = true;
  }
  if (written) {
    writtenLamps = lamps;
  }
}

void resetWrittenLamps() {
  writtenLamps = -1;
}
