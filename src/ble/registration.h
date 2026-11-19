// Registration of the BLE keyboard: the bonds (pairing information) that the NimBLE host keeps in the NVS, and the
// deletion of the registration (see SPEC.md)

#pragma once

#include <EspBle.h>

// Most bonds that the NimBLE host keeps
const size_t BOND_CAPACITY = CONFIG_BT_NIMBLE_MAX_BONDS;

// Whether the two bonds are of the same device
bool isSameBond(const EspBleBond &a, const EspBleBond &b);

// Read all the bonds (false if they cannot be read)
bool readBonds(EspBleBond *bonds, size_t &count);

// Delete the bond of a device that is not connected, even while connected to another one
bool deleteOldBond(const EspBleBond &bond);

// The registered keyboard: the only bond (none if there is no bond or more than one)
bool readRegisteredKeyboard(EspBleBond &keyboard);

// Start deleting the registration (false while pairing). It is deleted by deleteRegistration()
bool startRegistrationDeletion();

// Whether the deletion of the registration has been requested and is not done yet
bool isDeletingRegistration();

// Delete the registration once there is neither a connection nor an attempt to connect (called in loop())
void deleteRegistration();
