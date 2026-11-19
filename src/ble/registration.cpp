// Registration of the BLE keyboard (see registration.h)

#include "registration.h"

#include "address.h"
#include "ble.h"
#include "connection.h"
#include "pairing.h"
#include "storage.h"

// Deletion of the registration requested: done once there is neither a connection nor an attempt to connect
static bool registrationDeleting;

// Whether the two bonds are of the same device
bool isSameBond(const EspBleBond &a, const EspBleBond &b) {
  return a.peerAddressType == b.peerAddressType && a.peerAddress.equalsIgnoreCase(b.peerAddress);
}

// Read all the bonds (false if they cannot be read)
bool readBonds(EspBleBond *bonds, size_t &count) {
  count = min(ble.bondCount(), BOND_CAPACITY);
  for (size_t i = 0; i < count; i++) {
    if (!ble.bond(i, bonds[i])) {
      return false;
    }
  }
  return true;
}

// Delete the bond of a device that is not connected, even while connected to another one. EspBle's deleteBond() refuses
// while any connection is open, but NimBLE only deletes what is stored for the device, without touching the
// connections (ble_gap_unpair() does the same after closing only the connection to that device)
bool deleteOldBond(const EspBleBond &bond) {
  ble_addr_t peers[BOND_CAPACITY];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, static_cast<int>(BOND_CAPACITY)) != 0) {
    return false;
  }
  for (int i = 0; i < count; i++) {
    if (peers[i].type == static_cast<uint8_t>(bond.peerAddressType) &&
        formatAddress(peers[i].val).equalsIgnoreCase(bond.peerAddress)) {
      return ble_store_util_delete_peer(&peers[i]) == 0;
    }
  }
  return false;
}

// The registered keyboard: the only bond (none if there is no bond or more than one, see SPEC.md)
bool readRegisteredKeyboard(EspBleBond &keyboard) {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count) || count != 1) {
    return false;
  }
  keyboard = bonds[0];
  return true;
}

// Start deleting the registration (see SPEC.md): close the connection to the registered keyboard. The registration is
// deleted in loop() by deleteRegistration(), once there is neither a connection nor an attempt to connect
bool startRegistrationDeletion() {
  if (isPairing()) {
    return false;
  }
  registrationDeleting = true;
  if (keyboardConnectionId != 0) {
    ble.disconnect(keyboardConnectionId);
  }
  return true;
}

// Delete the registration once there is neither a connection nor an attempt to connect: all the bonds, and the name
// and the HID service saved with the registration. A connection made after the bond has been deleted would start a
// pairing, so an attempt to connect is waited for (see SPEC.md)
void deleteRegistration() {
  if (!registrationDeleting || keyboardConnecting || keyboardConnectionId != 0 || ble.connectionCount() != 0) {
    return;
  }
  registrationDeleting = false;
  ble.deleteAllBonds();
  forgetKeyboardName();
  forgetHidCache();
}

bool isDeletingRegistration() {
  return registrationDeleting;
}
