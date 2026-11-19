// Relay of the input of the registered keyboard to the host, and of the lamps of the host to the keyboard (see SPEC.md)

#pragma once

#include "gatt.h"

// Relay a notification of the registered keyboard: a keyboard input report or a media key report
void relayNotification(const NimbleEvent &event);

// Write the lamps of the host to the lamp output reports of the keyboard when they change (called in loop())
void writeLamps();

// Forget the lamps written, so that they are written again once the lamp output reports are ready on a new connection
void resetWrittenLamps();
