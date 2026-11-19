// Connection to the registered keyboard, whose input is relayed to the host (see SPEC.md)

#pragma once

#include <EspBle.h>

// State of the connection to the registered keyboard (the values are those of SPEC.md)
enum KeyboardState : uint8_t {
  KEYBOARD_IDLE = 0,        // Not connecting (including while searching or pairing)
  KEYBOARD_CONNECTING = 1,  // Trying to connect
  KEYBOARD_PREPARING = 2,   // Connected, encrypting and preparing the HID service
  KEYBOARD_RELAYING = 3,    // Relaying the input
};

// The connection, shared with the preparation of the HID service, the pairing and the relay in src/ble/
extern bool keyboardConnecting;                  // An attempt to connect is in progress
extern EspBleConnectionId keyboardConnectionId;  // 0 while not connected
extern bool keyboardReady;  // Subscribed to the keyboard input after the encryption with the bond: relaying
extern EspBleBond keyboardTarget;                // The registered keyboard that the attempt connects to
extern uint16_t keyboardConnectionHandle;        // NimBLE handle of the connection
extern bool keyboardUsesCache;                   // Uses the remembered HID service instead of discovering it
extern bool keyboardShortInterval;               // A short interval has been requested for the preparation
// Parameters of the connection when it was made (units of EspBleConnection), put back when the relay starts
extern uint16_t keyboardInterval;
extern uint16_t keyboardLatency;
extern uint16_t keyboardSupervisionTimeout;

// Receive the events of the connections, for the registered keyboard or for the device to pair with
void setupConnection();

// The state of the connection to the registered keyboard
KeyboardState keyboardState();

// Keep trying to connect to the registered keyboard, but not while searching, pairing or deleting the registration
// (called in loop())
void connectKeyboard();

// Ask for a short connection interval for the preparation of the HID service
bool askShortInterval(EspBleConnectionId id, uint16_t supervisionTimeout);

// The relay starts: put back the parameters of the connection, and remember the HID service if it was discovered
void startRelaying();

// Relay on the connection of the pairing, which is encrypted with the bond now registered and whose HID service is
// found. The connection is as it was when bonded
void relayPairingConnection(const EspBleBond &registered, EspBleConnectionId id, const EspBleConnection &connection,
                            bool shortInterval);
