// GATT operations made with NimBLE directly: their results and the notifications arrive in the NimBLE host task, and
// are passed to loop()

#pragma once

#include <host/ble_hs.h>

// Longest value passed to loop(). A longer notification is not relayed (a keyboard input report is far shorter)
const size_t NIMBLE_EVENT_MAX_DATA = 64;

// Notifications and results of the GATT operations made with NimBLE directly
enum NimbleEventType : uint8_t {
  NIMBLE_NOTIFICATION,  // A notification or an indication
  NIMBLE_DESCRIPTOR,    // A descriptor found by a discovery
  NIMBLE_DONE,          // An operation has ended (status 0 on success); the value for a read
};
struct NimbleEvent {
  NimbleEventType type;
  uint32_t operation;  // Tag of the operation (0 for a notification)
  uint16_t connectionHandle;
  uint16_t handle;     // Of the attribute
  int status;          // 0 on success, otherwise a NimBLE error
  uint16_t uuid16;     // NIMBLE_DESCRIPTOR: the UUID if it is a 16-bit one, otherwise 0
  uint8_t length;
  uint8_t data[NIMBLE_EVENT_MAX_DATA];
};

// Receive the notifications (after ble.begin())
void setupGatt();

// Tag of a new GATT operation, whose result is awaited from now on (the results of the earlier ones are ignored)
void *nextOperationTag();

// NimBLE host task: callbacks of ble_gattc_disc_all_dscs(), and of ble_gattc_read() or ble_gattc_write_flat()
int onNimbleDescriptor(uint16_t connectionHandle, const struct ble_gatt_error *error, uint16_t valueHandle,
                       const struct ble_gatt_dsc *descriptor, void *tag);
int onNimbleAttribute(uint16_t connectionHandle, const struct ble_gatt_error *error, struct ble_gatt_attr *attribute,
                      void *tag);

// Handle the notifications and the results of the GATT operations (called in loop())
void handleNimbleEvents();
