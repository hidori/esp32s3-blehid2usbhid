// BLE side of the bridge (see ble.h)

#include "ble.h"

#include "gatt.h"
#include "relay.h"
#include "service.h"

EspBle ble;

// The NVS could not be initialized, or BLE could not be started
static bool bleFailed;

void setupBle() {
  // Before anything else uses the NVS (the BLE bonds are kept there)
  bool nvsReady = initNvs();
  loadKeyboardName();
  loadHidCache();

  // Pair as the side that only displays the passkey, requesting MITM protection: a keyboard that can type gets
  // Passkey Entry (the passkey is shown on the settings page), and the others get Just Works
  EspBleConfig config;
  config.security.enabled = true;
  config.security.bonding = true;
  config.security.mitm = true;
  config.security.ioCapability = EspBleSecurityIoCapability::DisplayOnly;
  // The scan results and the events of the connections are delivered from ble.update() in updateBle()
  bool started = ble.begin(config);
  bleFailed = !nvsReady || !started;
  setupGatt();
  setupScan();
  setupConnection();
  setupPairing();
  setupService();
}

void updateBle() {
  ble.update();
  handleNimbleEvents();
  startRequestedScan();
  preparePairing();
  deleteRegistration();
  connectKeyboard();
  writeLamps();
}

bool hasBleFailed() {
  return bleFailed;
}
