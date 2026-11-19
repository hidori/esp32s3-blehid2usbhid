// Reports waiting to be sent to the host (see reports.h)

#include "reports.h"

#include <stddef.h>
#include <string.h>

#include "device.h"

// Keys in a boot keyboard report, and the reports waiting to be sent to the host
static const size_t KEYBOARD_REPORT_KEYS = 6;
static const size_t KEYBOARD_REPORT_QUEUE_SIZE = 16;

// Media key reports waiting to be sent to the host
static const size_t CONSUMER_REPORT_QUEUE_SIZE = 16;

// Reports waiting to be sent to the host. Notifications from the keyboard can arrive back to back, before the host has
// taken the previous report; dropping one would lose a key or leave a key pressed. A report is queued unless it is the
// state the host will already have. When the queue is full, the last report is replaced, so that the host still ends
// up in the latest state
template <typename Report, size_t SIZE>
struct ReportQueue {
  Report reports[SIZE];
  size_t head;
  size_t count;
  Report last;  // What the host will have when the queue has been sent

  void push(const Report &report) {
    if (memcmp(&report, &last, sizeof(report)) == 0) {
      return;
    }
    last = report;
    if (count == SIZE) {
      reports[(head + count - 1) % SIZE] = report;
      return;
    }
    reports[(head + count) % SIZE] = report;
    count++;
  }

  const Report &front() const { return reports[head]; }

  void pop() {
    head = (head + 1) % SIZE;
    count--;
  }
};

// Boot keyboard report
struct BootKeyboardReport {
  uint8_t modifiers;
  uint8_t keys[KEYBOARD_REPORT_KEYS];
};
static ReportQueue<BootKeyboardReport, KEYBOARD_REPORT_QUEUE_SIZE> keyboardQueue;

// Media key reports (the usage of the media key pressed, 0 for none)
static ReportQueue<uint16_t, CONSUMER_REPORT_QUEUE_SIZE> consumerQueue;

void queueKeyboardReport(uint8_t modifiers, uint8_t const *keys) {
  BootKeyboardReport report;
  report.modifiers = modifiers;
  memcpy(report.keys, keys, sizeof(report.keys));
  keyboardQueue.push(report);
}

void queueMediaKey(uint16_t usage) {
  consumerQueue.push(usage);
}

uint16_t lastQueuedMediaKey() {
  return consumerQueue.last;
}

void queueReleaseAll() {
  static const uint8_t NO_KEYS[KEYBOARD_REPORT_KEYS] = {};
  queueKeyboardReport(0, NO_KEYS);
  queueMediaKey(0);
}

// The keyboard reports are sent on the keyboard, and the media key reports on the settings HID. While the host is not
// ready (e.g. suspended), the reports are discarded
void sendReports() {
  if (!isHostReady()) {
    keyboardQueue.count = 0;
    consumerQueue.count = 0;
    return;
  }
  if (keyboardQueue.count > 0) {
    BootKeyboardReport report = keyboardQueue.front();
    if (sendKeyboardReport(report.modifiers, report.keys)) {
      keyboardQueue.pop();
    }
  }
  if (consumerQueue.count > 0 && sendMediaKey(consumerQueue.front())) {
    consumerQueue.pop();
  }
}
