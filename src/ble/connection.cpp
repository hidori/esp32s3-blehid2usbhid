// Connection to the registered keyboard (see connection.h)

#include "connection.h"

#include "../usb/usb.h"
#include "ble.h"
#include "relay.h"
#include "service.h"

// Length of an attempt to connect to the registered keyboard. The attempts follow one another, so this does not delay
// the connection; it only bounds how long the start of a search waits for an attempt to end
static const uint32_t KEYBOARD_CONNECT_ATTEMPT_MS = 1000;

// Connection interval requested while the HID service of the registered keyboard is prepared, in units of 1.25 ms
// (7.5-15 ms). Each GATT operation takes round trips over the connection, so a short interval shortens the
// preparation. The parameters of the connection are put back when the relay starts
static const uint16_t PREPARATION_INTERVAL_MIN = 6;
static const uint16_t PREPARATION_INTERVAL_MAX = 12;

// The connection (see connection.h)
bool keyboardConnecting;
EspBleConnectionId keyboardConnectionId;
bool keyboardReady;
EspBleBond keyboardTarget;
uint16_t keyboardConnectionHandle;
bool keyboardUsesCache;
bool keyboardShortInterval;
uint16_t keyboardInterval;
uint16_t keyboardLatency;
uint16_t keyboardSupervisionTimeout;

// The state of the connection to the registered keyboard (see SPEC.md)
KeyboardState keyboardState() {
  if (keyboardConnectionId != 0) {
    return keyboardReady ? KEYBOARD_RELAYING : KEYBOARD_PREPARING;
  }
  return keyboardConnecting ? KEYBOARD_CONNECTING : KEYBOARD_IDLE;
}

// Keep trying to connect to the registered keyboard, but not while searching, pairing or deleting the registration (see
// SPEC.md)
void connectKeyboard() {
  if (keyboardConnecting || keyboardConnectionId != 0 || isPairing() || isScanning() || isDeletingRegistration()) {
    return;
  }
  EspBleBond registered;
  if (!readRegisteredKeyboard(registered)) {
    return;
  }
  keyboardTarget = registered;
  keyboardConnecting =
      ble.connect(registered.peerAddress.c_str(), registered.peerAddressType, KEYBOARD_CONNECT_ATTEMPT_MS);
}

// Ask for a short connection interval for the preparation of the HID service (see PREPARATION_INTERVAL_MIN)
bool askShortInterval(EspBleConnectionId id, uint16_t supervisionTimeout) {
  return ble.updateConnectionParameters(id, PREPARATION_INTERVAL_MIN, PREPARATION_INTERVAL_MAX, 0, supervisionTimeout);
}

// The relay starts. The parameters of the connection are put back if they were changed for the preparation, and the
// HID service is remembered if it was discovered
void startRelaying() {
  keyboardReady = true;
  // So that the relay runs as before the preparation was shortened
  if (keyboardShortInterval) {
    ble.updateConnectionParameters(keyboardConnectionId, keyboardInterval, keyboardInterval, keyboardLatency,
                                   keyboardSupervisionTimeout);
  }
  if (!keyboardUsesCache) {
    static HidCache cache;
    if (buildHidCache(cache)) {
      saveHidCache(cache, keyboardTarget);
    }
  }
}

// Relay on the connection of the pairing, which is encrypted with the bond now registered and whose HID service is
// found. Closing it would make some keyboards wait for a key before they accept a connection again (see SPEC.md)
void relayPairingConnection(const EspBleBond &registered, EspBleConnectionId id, const EspBleConnection &connection,
                            bool shortInterval) {
  keyboardConnectionId = id;
  keyboardConnectionHandle = connection.handle;
  keyboardTarget = registered;
  keyboardReady = false;
  // Not remembered again when the relay starts, unless it could not be saved with the registration
  keyboardUsesCache = hidCacheMatches(registered);
  keyboardShortInterval = shortInterval;
  keyboardInterval = connection.connectionInterval;
  keyboardLatency = connection.peripheralLatency;
  keyboardSupervisionTimeout = connection.supervisionTimeout;
  hidForPairing = false;
  resetWrittenLamps();
  // The Report References have just been read while pairing
  hidCheckReports = false;
  hidLatePhase = false;
  subscribeReports();
}

// Connected to the registered keyboard; the encryption with the bond starts by itself (pairOnConnect). Without a
// remembered HID service, ask for a short connection interval for its discovery. A remembered one needs only a few
// round trips, which end before a new interval would take effect. If the pairing or the deletion of the registration
// has started during the attempt, close the connection so that it can go on
static void onKeyboardConnected(const EspBleConnection &connection) {
  keyboardConnectionId = connection.id;
  keyboardConnectionHandle = connection.handle;
  keyboardReady = false;
  resetWrittenLamps();
  keyboardShortInterval = false;
  if (isPairing() || isDeletingRegistration()) {
    ble.disconnect(connection.id);
    return;
  }
  keyboardInterval = connection.connectionInterval;
  keyboardLatency = connection.peripheralLatency;
  keyboardSupervisionTimeout = connection.supervisionTimeout;
  keyboardUsesCache = hidCacheMatches(keyboardTarget);
  if (!keyboardUsesCache) {
    keyboardShortInterval = askShortInterval(connection.id, connection.supervisionTimeout);
  }
}

// Encrypted with the bond: prepare the HID service to receive the input, with the remembered one if any. Otherwise the
// device is not the registered keyboard (or has lost the keys), so close the connection
static void onKeyboardSecurityChanged(const EspBleSecurityChanged &event) {
  if (!event.success || !event.connection.bonded) {
    ble.disconnect(event.connection.id);
    return;
  }
  // Once for a connection
  if (hidConnectionId == keyboardConnectionId || keyboardReady) {
    return;
  }
  hidConnectionId = keyboardConnectionId;
  hidForPairing = false;
  hidLatePhase = false;
  if (!keyboardUsesCache) {
    startHidDiscovery();
  } else if (!useHidCache()) {
    rediscoverHidService();
  } else {
    hidCheckReports = true;
    prepareReports();
  }
}

// The connection to the registered keyboard is closed: release every key and media key on the host
static void onKeyboardDisconnected(const EspBleConnection &connection) {
  keyboardConnectionId = 0;
  keyboardReady = false;
  if (hidConnectionId == connection.id) {
    hidConnectionId = 0;
    hidStep = HID_STEP_NONE;
  }
  hidLatePhase = false;
  hidReportCount = 0;
  queueReleaseAll();
}

// Events of the connections, for the registered keyboard or for the device to pair with. Only one attempt to connect
// runs at a time, and the keyboard is not connected to while pairing
static void onConnected(const EspBleConnection &connection) {
  if (keyboardConnecting) {
    keyboardConnecting = false;
    onKeyboardConnected(connection);
  } else {
    onPairingConnected(connection);
  }
}

static void onConnectionFailed(const EspBleConnectionFailure &failure) {
  if (keyboardConnecting) {
    keyboardConnecting = false;
  } else {
    onPairingConnectionFailed(failure);
  }
}

static void onSecurityChanged(const EspBleSecurityChanged &event) {
  if (keyboardConnectionId != 0 && event.connection.id == keyboardConnectionId) {
    onKeyboardSecurityChanged(event);
  } else {
    onPairingSecurityChanged(event);
  }
}

static void onDisconnected(const EspBleConnection &connection) {
  if (keyboardConnectionId != 0 && connection.id == keyboardConnectionId) {
    onKeyboardDisconnected(connection);
  } else {
    onPairingDisconnected(connection);
  }
}

void setupConnection() {
  ble.onConnected(onConnected);
  ble.onConnectionFailed(onConnectionFailed);
  ble.onSecurityChanged(onSecurityChanged);
  ble.onDisconnected(onDisconnected);
}
