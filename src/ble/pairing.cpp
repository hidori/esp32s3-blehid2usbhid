// Pairing with a BLE keyboard found by the search (see pairing.h)

#include "pairing.h"

#include "address.h"
#include "ble.h"
#include "connection.h"
#include "registration.h"
#include "scan.h"
#include "service.h"
#include "storage.h"

static PairingState pairingState = PAIRING_IDLE;
static bool pairingPreparing;                         // Waiting for the relay to stop before connecting
static EspBleBond pairingTarget;                      // The device to pair with
static uint8_t pairingTargetNameSize;                 // Name of the device in the scan result
static char pairingTargetName[SCAN_NAME_MAX_SIZE];
static uint32_t pairingPasskey;                       // Valid in PAIRING_PASSKEY
static EspBleConnectionId pairingConnectionId;        // 0 while not connected
static EspBleConnection pairingConnection;            // As it was when bonded, handed over to the relay
static bool pairingShortInterval;                     // A short interval has been requested to find the HID service
static bool pairingCancelRequested;                   // Canceled; ends when the connection is closed
static bool pairingBonded;                            // Made a bond; registered before the relay or after closing
static bool pairingNotKeyboard;                       // The HID service shows no keyboard input: not registered
static EspBleBond bondsBeforePairing[BOND_CAPACITY];  // Bonds of the other devices when the pairing started
static size_t bondsBeforePairingCount;
static HidCache pairingCache;  // Found while pairing, saved when the registration has been replaced

// Whether the pairing is in progress (see SPEC.md)
bool isPairing() {
  return pairingState == PAIRING_CONNECTING || pairingState == PAIRING_PAIRING || pairingState == PAIRING_PASSKEY;
}

// Whether the bond was there when the pairing started
static bool isBondBeforePairing(const EspBleBond &bond) {
  for (size_t i = 0; i < bondsBeforePairingCount; i++) {
    if (isSameBond(bondsBeforePairing[i], bond)) {
      return true;
    }
  }
  return false;
}

// Leave only the bond made by the pairing, so that it replaces the registration. The others are deleted without closing
// the connection of the pairing
static bool replaceRegistration() {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return false;
  }
  bool made = false;
  for (size_t i = 0; i < count; i++) {
    made = made || !isBondBeforePairing(bonds[i]);
  }
  if (!made) {
    // The pairing has updated a bond with an address seen before: it can be told apart only if it is the only one
    return count == 1;
  }
  for (size_t i = 0; i < count; i++) {
    if (isBondBeforePairing(bonds[i]) && !deleteOldBond(bonds[i])) {
      return false;
    }
  }
  return true;
}

// Replace the registration with the bond made by the pairing, and save with it the name in the scan result and the HID
// service found while pairing. The name is only for showing, and the HID service is discovered on the first connection
// if it is not remembered, so a failure to save them does not undo the registration
static bool registerPairedKeyboard() {
  bool replaced = replaceRegistration();
  EspBleBond registered[BOND_CAPACITY];
  size_t registeredCount;
  if (replaced && readBonds(registered, registeredCount) && registeredCount == 1) {
    saveKeyboardName(registered[0], pairingTargetName, pairingTargetNameSize);
    if (pairingCache.version == HID_CACHE_VERSION) {
      saveHidCache(pairingCache, registered[0]);
    }
  }
  return replaced;
}

// Delete the bond made by the pairing, so that the registration stays as it was (needs no connection to delete a bond)
static bool deletePairingBond() {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (!isBondBeforePairing(bonds[i]) && !ble.deleteBond(bonds[i])) {
      return false;
    }
  }
  return true;
}

// Start pairing with the device in the scan result (see SPEC.md). The connection is made in loop() by
// preparePairing(), after the relay has stopped
PairingStart startPairing(uint8_t addressType, uint8_t const *address) {
  if (isPairing() || isDeletingRegistration()) {
    return PAIRING_BUSY;
  }
  const ScannedDevice *device = findScannedDevice(addressType, address);
  if (device == NULL) {
    return PAIRING_UNKNOWN_DEVICE;
  }
  pairingTarget.peerAddress = formatAddress(device->address);
  pairingTarget.peerAddressType = static_cast<EspBleAddressType>(device->addressType);
  pairingTargetNameSize = device->nameSize;
  memcpy(pairingTargetName, device->name, device->nameSize);
  pairingPasskey = 0;
  pairingConnectionId = 0;
  pairingCancelRequested = false;
  pairingBonded = false;
  pairingNotKeyboard = false;
  pairingCache.version = 0;
  pairingPreparing = true;
  pairingState = PAIRING_CONNECTING;
  cancelScanRequest();
  // A bond can be deleted only without a connection
  if (keyboardConnectionId != 0) {
    ble.disconnect(keyboardConnectionId);
  }
  return PAIRING_STARTED;
}

// Connect to the device to pair with once there is neither a connection nor an attempt to connect
void preparePairing() {
  if (!pairingPreparing) {
    return;
  }
  if (pairingCancelRequested) {
    pairingPreparing = false;
    pairingState = PAIRING_IDLE;
    return;
  }
  if (keyboardConnecting || keyboardConnectionId != 0 || ble.connectionCount() != 0) {
    return;
  }
  pairingPreparing = false;

  // Delete the bond of the device itself, or the stack would only encrypt with the old keys instead of pairing.
  // Keep the others until the pairing succeeds (see SPEC.md)
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    pairingState = PAIRING_REGISTRATION_FAILED;
    return;
  }
  bondsBeforePairingCount = 0;
  for (size_t i = 0; i < count; i++) {
    if (!isSameBond(bonds[i], pairingTarget)) {
      bondsBeforePairing[bondsBeforePairingCount++] = bonds[i];
    } else if (!ble.deleteBond(bonds[i])) {
      pairingState = PAIRING_REGISTRATION_FAILED;
      return;
    }
  }

  // connect() stops the search first, and the pairing starts as soon as connected (pairOnConnect)
  if (!ble.connect(pairingTarget.peerAddress.c_str(), pairingTarget.peerAddressType)) {
    pairingState = PAIRING_CONNECT_FAILED;
  }
}

// Cancel the pairing, which ends when the connection is closed (see SPEC.md)
void cancelPairing() {
  if (!isPairing()) {
    return;
  }
  pairingCancelRequested = true;
  // A connection attempt cannot be canceled: onPairingConnected() closes the connection if it is made, and
  // preparePairing() ends the preparation
  if (pairingConnectionId != 0) {
    ble.disconnect(pairingConnectionId);
  }
}

// Connected to the device to pair with
void onPairingConnected(const EspBleConnection &connection) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingConnectionId = connection.id;
  if (pairingCancelRequested) {
    ble.disconnect(connection.id);
    return;
  }
  pairingState = PAIRING_PAIRING;
}

// Failed to connect to the device to pair with
void onPairingConnectionFailed(const EspBleConnectionFailure &failure) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_CONNECT_FAILED;
}

// The passkey to be typed on the keyboard (Passkey Entry)
static void onPairingPasskey(const EspBlePasskeyDisplayed &event) {
  if (event.connection.id != pairingConnectionId || pairingCancelRequested) {
    return;
  }
  pairingPasskey = event.passkey;
  pairingState = PAIRING_PASSKEY;
}

// The pairing has ended. With a bond, the HID service is found to remember it and to see that the device is a
// keyboard, and then the registration is replaced and the connection goes on to the relay (see SPEC.md)
void onPairingSecurityChanged(const EspBleSecurityChanged &event) {
  if (event.connection.id != pairingConnectionId || hidConnectionId == pairingConnectionId) {
    return;
  }
  pairingBonded = event.success && event.connection.bonded;
  pairingState = PAIRING_PAIRING;
  if (pairingBonded && !pairingCancelRequested) {
    pairingConnection = event.connection;
    hidConnectionId = pairingConnectionId;
    hidForPairing = true;
    pairingShortInterval = askShortInterval(pairingConnectionId, event.connection.supervisionTimeout);
    startHidDiscovery();
    return;
  }
  ble.disconnect(pairingConnectionId);
}

// The connection to the device to pair with is closed before the relay has taken it over: the end of the pairing
void onPairingDisconnected(const EspBleConnection &connection) {
  if (pairingConnectionId == 0 || connection.id != pairingConnectionId) {
    return;
  }
  pairingConnectionId = 0;
  if (hidConnectionId == connection.id) {
    hidConnectionId = 0;
    hidStep = HID_STEP_NONE;
  }
  if (pairingBonded && pairingNotKeyboard) {
    pairingState = deletePairingBond() ? PAIRING_NOT_KEYBOARD : PAIRING_REGISTRATION_FAILED;
  } else if (pairingBonded) {
    pairingState = registerPairedKeyboard() ? PAIRING_REGISTERED : PAIRING_REGISTRATION_FAILED;
  } else {
    pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_PAIR_FAILED;
  }
}

// The HID service of the device paired with is found: remember it, replace the registration, and relay on this
// connection. If canceled, or if the registration cannot be replaced, close the connection instead, and the
// registration is replaced after the disconnection
void finishPairingPreparation() {
  hidStep = HID_STEP_NONE;
  buildHidCache(pairingCache);
  EspBleBond registered;
  if (pairingCancelRequested || !registerPairedKeyboard() || !readRegisteredKeyboard(registered)) {
    ble.disconnect(hidConnectionId);
    return;
  }
  pairingState = PAIRING_REGISTERED;
  EspBleConnectionId id = pairingConnectionId;
  pairingConnectionId = 0;
  relayPairingConnection(registered, id, pairingConnection, pairingShortInterval);
}

void setupPairing() {
  ble.onPasskeyDisplayed(onPairingPasskey);
}

PairingState getPairingState() {
  return pairingState;
}

uint32_t getPairingPasskey() {
  return pairingPasskey;
}

void setPairingNotKeyboard(bool notKeyboard) {
  pairingNotKeyboard = notKeyboard;
}
