// Pairing with a BLE keyboard found by the search, and the replacement of the registration (see SPEC.md)

#pragma once

#include <EspBle.h>

// Pairing state (the values are those of SPEC.md)
enum PairingState : uint8_t {
  PAIRING_IDLE = 0,                 // Not pairing
  PAIRING_CONNECTING = 1,           // Connecting (also while preparing to connect)
  PAIRING_PAIRING = 2,              // Pairing (also while the registration is replaced after the pairing)
  PAIRING_PASSKEY = 3,              // Waiting for the passkey to be typed on the keyboard
  PAIRING_REGISTERED = 4,           // Registered
  PAIRING_CONNECT_FAILED = 5,       // Failed to connect
  PAIRING_PAIR_FAILED = 6,          // Failed to pair
  PAIRING_REGISTRATION_FAILED = 7,  // Failed to rewrite the registration
  PAIRING_NOT_KEYBOARD = 8,         // Not registered, since the device has no keyboard input
};

// Result of starting a pairing
enum PairingStart : uint8_t {
  PAIRING_STARTED,
  PAIRING_BUSY,            // Pairing, or deleting the registration
  PAIRING_UNKNOWN_DEVICE,  // Not in the scan result
};

// Receive the passkey to be typed on the keyboard
void setupPairing();

// Whether the pairing is in progress
bool isPairing();

// The pairing state, and the passkey to be typed on the keyboard (valid in PAIRING_PASSKEY)
PairingState getPairingState();
uint32_t getPairingPasskey();

// Start pairing with the device in the scan result. The connection is made in loop() by preparePairing(), after the
// relay has stopped
PairingStart startPairing(uint8_t addressType, uint8_t const *address);

// Connect to the device to pair with once there is neither a connection nor an attempt to connect (called in loop())
void preparePairing();

// Cancel the pairing, which ends when the connection is closed
void cancelPairing();

// Events of the connection to the device to pair with
void onPairingConnected(const EspBleConnection &connection);
void onPairingConnectionFailed(const EspBleConnectionFailure &failure);
void onPairingSecurityChanged(const EspBleSecurityChanged &event);
void onPairingDisconnected(const EspBleConnection &connection);

// The HID service of the device paired with has been found (see service.h): remember it, replace the registration,
// and relay on this connection
void finishPairingPreparation();

// The HID service of the device paired with shows that the device is not a keyboard: it is not registered
void setPairingNotKeyboard(bool notKeyboard);
