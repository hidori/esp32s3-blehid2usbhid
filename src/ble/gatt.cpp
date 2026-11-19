// GATT operations made with NimBLE directly (see gatt.h)

#include "gatt.h"

#include "relay.h"
#include "service.h"

// Notifications and results waiting to be handled in loop()
static const size_t NIMBLE_EVENT_QUEUE_SIZE = 32;
static QueueHandle_t nimbleEvents;
static struct ble_gap_event_listener nimbleListener;
static uint32_t nimbleOperation;  // Tag of the operation whose result is awaited

// Tag passed to a NimBLE GATT operation, and back to loop() with its result
static void *nimbleOperationTag(uint32_t operation) {
  return reinterpret_cast<void *>(static_cast<uintptr_t>(operation));
}

static uint32_t nimbleOperationOf(void *tag) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tag));
}

// The NimBLE host task passes the events to loop() (an event is lost if the queue is full)
static void pushNimbleEvent(const NimbleEvent &event) {
  xQueueSend(nimbleEvents, &event, 0);
}

// Copy the value of an mbuf into the event (status BLE_HS_EMSGSIZE if it is too long)
static void copyNimbleData(const struct os_mbuf *om, NimbleEvent &event) {
  uint16_t length = om == NULL ? 0 : OS_MBUF_PKTLEN(om);
  uint16_t copied = 0;
  if (length > NIMBLE_EVENT_MAX_DATA || (length > 0 && ble_hs_mbuf_to_flat(om, event.data, length, &copied) != 0)) {
    event.status = BLE_HS_EMSGSIZE;
    return;
  }
  event.length = copied;
}

// NimBLE host task: a notification or an indication from a peer (EspBle passes on only those it subscribed to)
static int onNimbleGapEvent(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_NOTIFY_RX) {
    return 0;
  }
  NimbleEvent result = {};
  result.type = NIMBLE_NOTIFICATION;
  result.connectionHandle = event->notify_rx.conn_handle;
  result.handle = event->notify_rx.attr_handle;
  copyNimbleData(event->notify_rx.om, result);
  pushNimbleEvent(result);
  return 0;
}

// NimBLE host task: a descriptor found by ble_gattc_disc_all_dscs(), or the end of the discovery
int onNimbleDescriptor(uint16_t connectionHandle, const struct ble_gatt_error *error, uint16_t valueHandle,
                       const struct ble_gatt_dsc *descriptor, void *tag) {
  NimbleEvent result = {};
  result.operation = nimbleOperationOf(tag);
  result.connectionHandle = connectionHandle;
  if (error->status == 0 && descriptor != NULL) {
    result.type = NIMBLE_DESCRIPTOR;
    result.handle = descriptor->handle;
    result.uuid16 = descriptor->uuid.u.type == BLE_UUID_TYPE_16 ? descriptor->uuid.u16.value : 0;
  } else {
    result.type = NIMBLE_DONE;
    result.status = error->status == BLE_HS_EDONE ? 0 : error->status;
  }
  pushNimbleEvent(result);
  return 0;
}

// NimBLE host task: the result of ble_gattc_read() or ble_gattc_write_flat()
int onNimbleAttribute(uint16_t connectionHandle, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attribute, void *tag) {
  NimbleEvent result = {};
  result.type = NIMBLE_DONE;
  result.operation = nimbleOperationOf(tag);
  result.connectionHandle = connectionHandle;
  result.status = error->status;
  if (error->status == 0 && attribute != NULL) {
    result.handle = attribute->handle;
    copyNimbleData(attribute->om, result);
  }
  pushNimbleEvent(result);
  return 0;
}

// Handle the notifications and the results of the NimBLE GATT operations in loop(). A result of an operation that is
// no longer awaited (e.g. one of a closed connection) is ignored
void handleNimbleEvents() {
  NimbleEvent event;
  while (xQueueReceive(nimbleEvents, &event, 0) == pdTRUE) {
    if (event.type == NIMBLE_NOTIFICATION) {
      relayNotification(event);
    } else if (event.operation == nimbleOperation) {
      onNimbleResult(event);
    }
  }
}

void setupGatt() {
  nimbleEvents = xQueueCreate(NIMBLE_EVENT_QUEUE_SIZE, sizeof(NimbleEvent));
  // The keyboard input reports are subscribed to with NimBLE directly, so their notifications are received here too
  ble_gap_event_listener_register(&nimbleListener, onNimbleGapEvent, NULL);
}

void *nextOperationTag() {
  return nimbleOperationTag(++nimbleOperation);
}
