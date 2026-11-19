// BLE side of the bridge: the search, the pairing and the registration of the BLE keyboard, and the relay of its input
// (the entry of src/ble/)

#pragma once

#include <EspBle.h>

#include "address.h"
#include "connection.h"
#include "pairing.h"
#include "registration.h"
#include "scan.h"
#include "storage.h"

// The BLE stack, shared in src/ble/
extern EspBle ble;

// Set up the NVS and BLE
void setupBle();

// Run the BLE side: the events, the search, the pairing, the deletion of the registration, the connection to the
// registered keyboard and the lamps (called in loop())
void updateBle();
