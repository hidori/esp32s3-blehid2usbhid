// Stub of the device in place of WebHID, for checking the settings page in the browser of the Playwright MCP server.
// Loaded with --init-script before the scripts of the page. It answers the requests like the firmware (see SPEC.md).

(() => {
  // Like real browsers, WebHID is available only in a secure context
  if (!window.isSecureContext) {
    return;
  }

  // Settings HID protocol (see SPEC.md)
  const REPORT_ID = 2;
  const REPORT_SIZE = 63;
  const COMMAND_RESTART = 0x01;
  const COMMAND_START_SCAN = 0x04;
  const COMMAND_STOP_SCAN = 0x05;
  const COMMAND_GET_SCAN_RESULT = 0x06;
  const COMMAND_START_PAIRING = 0x07;
  const COMMAND_CANCEL_PAIRING = 0x08;
  const COMMAND_GET_PAIRING_STATE = 0x09;
  const COMMAND_GET_REGISTRATION = 0x0a;
  const RESULT_SUCCESS = 0x00;
  const RESULT_UNKNOWN_COMMAND = 0x01;
  const RESULT_INVALID_ARGUMENT = 0x02;
  const RESULT_BLE_FAILED = 0x04;
  const PAIRING_IDLE = 0;
  const PAIRING_CONNECTING = 1;
  const PAIRING_PAIRING = 2;
  const PAIRING_PASSKEY = 3;
  const PAIRING_REGISTERED = 4;
  const PAIRING_CONNECT_FAILED = 5;
  const PAIRING_PAIR_FAILED = 6;
  const PAIRING_NOT_KEYBOARD = 8;
  const KEYBOARD_IDLE = 0;
  const KEYBOARD_CONNECTING = 1;
  const KEYBOARD_PREPARING = 2;
  const KEYBOARD_RELAYING = 3;

  // Time until a response arrives, and until the device is disconnected by a restart
  const RESPONSE_DELAY_MS = 10;
  const RESTART_DELAY_MS = 100;

  // BLE keyboards found by the search, each after the time from the start of the search, and how the pairing with
  // each goes (see PAIRING_TIMELINES). The addresses are little endian as in the scan result (the first one is shown
  // as C6:55:44:33:22:11)
  const SCAN_DEVICES = [
    {
      afterMs: 300,
      addressType: 1,
      address: [0x11, 0x22, 0x33, 0x44, 0x55, 0xc6],
      rssi: -52,
      name: "Stub Keyboard",
      pairing: "passkey",
    },
    {
      afterMs: 1500,
      addressType: 0,
      address: [0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb],
      rssi: -71,
      name: "",
      pairing: "justWorks",
    },
    {
      afterMs: 3000,
      addressType: 1,
      address: [0x01, 0x02, 0x03, 0x04, 0x05, 0xd6],
      rssi: -64,
      name: "スタブのキーボード",
      pairing: "wrongPasskey",
    },
    {
      afterMs: 4500,
      addressType: 1,
      address: [0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb],
      rssi: -90,
      name: "Stub Faraway",
      pairing: "unreachable",
    },
    {
      afterMs: 6000,
      addressType: 1,
      address: [0x10, 0x32, 0x54, 0x76, 0x98, 0xda],
      rssi: -58,
      name: "Stub Mouse",
      pairing: "notKeyboard",
    },
  ];

  // How each pairing goes: the states until the times from the start of the pairing, then the end state.
  // The first state is always connecting (shorter than the 10 s connection timeout of the firmware)
  const PAIRING_TIMELINES = {
    passkey: {
      steps: [[1000, PAIRING_CONNECTING], [1500, PAIRING_PAIRING], [6500, PAIRING_PASSKEY]],
      end: PAIRING_REGISTERED,
    },
    justWorks: {
      steps: [[1000, PAIRING_CONNECTING], [2500, PAIRING_PAIRING]],
      end: PAIRING_REGISTERED,
    },
    wrongPasskey: {
      steps: [[1000, PAIRING_CONNECTING], [1500, PAIRING_PAIRING], [4500, PAIRING_PASSKEY]],
      end: PAIRING_PAIR_FAILED,
    },
    unreachable: {
      steps: [[3000, PAIRING_CONNECTING]],
      end: PAIRING_CONNECT_FAILED,
    },
    // Paired (Just Works), but the HID service has no keyboard input
    notKeyboard: {
      steps: [[1000, PAIRING_CONNECTING], [3500, PAIRING_PAIRING]],
      end: PAIRING_NOT_KEYBOARD,
    },
  };

  // Passkey shown while waiting for it to be typed (with leading zeros, to check that they are shown)
  const PASSKEY = 4321;

  // Time until the connection is closed after a cancel
  const DISCONNECT_MS = 100;

  // Time until the relay starts on the connection of the pairing, and until the registered keyboard is connected to
  // after a restart
  const RELAY_START_MS = 300;
  const RECONNECT_MS = 1500;

  // Search: when it started (null before the first search) and when it stopped (null while searching)
  let scanStartedMs = null;
  let scanStoppedMs = null;

  // Pairing: { found, timeline, startedMs, endedMs, registeredChecked } (null before the first pairing). endedMs is
  // when a canceled pairing ends
  let pairing = null;

  // Registered keyboard: { found, sinceMs, waitState, waitMs } (null if none). It survives a restart like the bonds in
  // the NVS. The connection is in waitState for waitMs after sinceMs, and relays after that
  let registered = null;

  // Devices found by the search so far
  function scannedDevices() {
    if (scanStartedMs === null) {
      return [];
    }
    const elapsedMs = (scanStoppedMs ?? performance.now()) - scanStartedMs;
    return SCAN_DEVICES.filter((found) => found.afterMs <= elapsedMs);
  }

  // Scan result data for the device at the index (see SPEC.md)
  function scanResult(index) {
    const devices = scannedDevices();
    const scanning = scanStartedMs !== null && scanStoppedMs === null;
    if (index >= devices.length) {
      return new Uint8Array([scanning ? 1 : 0, devices.length]);
    }
    const found = devices[index];
    const name = new TextEncoder().encode(found.name);
    return new Uint8Array([
      scanning ? 1 : 0, devices.length, found.addressType, ...found.address, found.rssi & 0xff, name.length, ...name,
    ]);
  }

  // Pairing state like the firmware (see SPEC.md)
  function pairingState() {
    if (pairing === null) {
      return PAIRING_IDLE;
    }
    const now = performance.now();
    if (pairing.endedMs !== null && now >= pairing.endedMs) {
      return PAIRING_IDLE;
    }
    const elapsedMs = now - pairing.startedMs;
    const step = pairing.timeline.steps.find(([untilMs]) => elapsedMs < untilMs);
    return step ? step[1] : pairing.timeline.end;
  }

  // Whether the pairing is in progress
  function isPairing() {
    const state = pairingState();
    return state === PAIRING_CONNECTING || state === PAIRING_PAIRING || state === PAIRING_PASSKEY;
  }

  // Pairing state data (see SPEC.md)
  function pairingStateData() {
    const state = pairingState();
    const data = new DataView(new ArrayBuffer(5));
    data.setUint8(0, state);
    data.setUint32(1, state === PAIRING_PASSKEY ? PASSKEY : 0, true);
    return new Uint8Array(data.buffer);
  }

  // Start pairing with the device in the scan result
  function startPairing(args) {
    if (isPairing()) {
      return RESULT_BLE_FAILED;
    }
    const found = scannedDevices().find(
      (candidate) => candidate.addressType === args[0] && candidate.address.every((byte, i) => byte === args[1 + i]),
    );
    if (!found) {
      return RESULT_INVALID_ARGUMENT;
    }
    // Like EspBle, the search is stopped before connecting
    if (scanStartedMs !== null && scanStoppedMs === null) {
      scanStoppedMs = performance.now();
    }
    pairing = {
      found,
      timeline: PAIRING_TIMELINES[found.pairing],
      startedMs: performance.now(),
      endedMs: null,
      registeredChecked: false,
    };
    return RESULT_SUCCESS;
  }

  // Cancel the pairing: a connection attempt is not canceled but runs out, and a connection is closed
  function cancelPairing() {
    if (!isPairing() || pairing.endedMs !== null) {
      return;
    }
    if (pairingState() === PAIRING_CONNECTING) {
      pairing.endedMs = pairing.startedMs + pairing.timeline.steps[0][0];
    } else {
      pairing.endedMs = performance.now() + DISCONNECT_MS;
    }
  }

  // Register the device of the pairing once the pairing has succeeded
  function updateRegistered() {
    if (pairing === null || pairing.registeredChecked || pairingState() !== PAIRING_REGISTERED) {
      return;
    }
    const steps = pairing.timeline.steps;
    // The connection of the pairing goes on to the relay
    registered = {
      found: pairing.found,
      sinceMs: pairing.startedMs + steps[steps.length - 1][0],
      waitState: KEYBOARD_PREPARING,
      waitMs: RELAY_START_MS,
    };
    pairing.registeredChecked = true;
  }

  // Registration data like the firmware (see SPEC.md)
  function registrationData() {
    updateRegistered();
    if (registered === null) {
      return new Uint8Array([0]);
    }
    let state;
    if (isPairing() || (scanStartedMs !== null && scanStoppedMs === null)) {
      state = KEYBOARD_IDLE; // Not connected to while searching or pairing
    } else {
      state = performance.now() - registered.sinceMs < registered.waitMs ? registered.waitState : KEYBOARD_RELAYING;
    }
    const name = new TextEncoder().encode(registered.found.name);
    return new Uint8Array([1, registered.found.addressType, ...registered.found.address, state, name.length, ...name]);
  }

  class StubDevice extends EventTarget {
    productName = "BLE HID to USB HID Bridge (STUB)";
    vendorId = 0x303a;
    productId = 0x4005;
    opened = false;

    async open() {
      this.opened = true;
    }

    async close() {
      this.opened = false;
    }

    async sendReport(reportId, data) {
      if (!this.opened) {
        throw new DOMException("The device must be opened first.", "InvalidStateError");
      }
      const request = ArrayBuffer.isView(data)
        ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
        : new Uint8Array(data);
      if (reportId === REPORT_ID) {
        setTimeout(() => this.handleRequest(request), RESPONSE_DELAY_MS);
      }
    }

    // Answer a request like the firmware
    handleRequest(request) {
      const command = request[0];
      switch (command) {
        case COMMAND_RESTART:
          this.respond(command, RESULT_SUCCESS);
          setTimeout(() => this.disconnect(), RESTART_DELAY_MS);
          break;
        case COMMAND_START_SCAN:
          // Not while pairing, like the firmware
          if (isPairing()) {
            this.respond(command, RESULT_BLE_FAILED);
            break;
          }
          scanStartedMs = performance.now();
          scanStoppedMs = null;
          this.respond(command, RESULT_SUCCESS);
          break;
        case COMMAND_STOP_SCAN:
          if (scanStartedMs !== null && scanStoppedMs === null) {
            scanStoppedMs = performance.now();
          }
          this.respond(command, RESULT_SUCCESS);
          break;
        case COMMAND_GET_SCAN_RESULT:
          this.respond(command, RESULT_SUCCESS, scanResult(request[1]));
          break;
        case COMMAND_START_PAIRING:
          this.respond(command, startPairing(request.subarray(1)));
          break;
        case COMMAND_CANCEL_PAIRING:
          cancelPairing();
          this.respond(command, RESULT_SUCCESS);
          break;
        case COMMAND_GET_PAIRING_STATE:
          this.respond(command, RESULT_SUCCESS, pairingStateData());
          break;
        case COMMAND_GET_REGISTRATION:
          this.respond(command, RESULT_SUCCESS, registrationData());
          break;
        default:
          this.respond(command, RESULT_UNKNOWN_COMMAND);
          break;
      }
    }

    // Send a response as an input report: [command, result, data...]
    respond(command, result, data = new Uint8Array(0)) {
      if (!this.opened) {
        return;
      }
      const report = new Uint8Array(REPORT_SIZE);
      report[0] = command;
      report[1] = result;
      report.set(data, 2);
      const event = new Event("inputreport");
      Object.assign(event, { device: this, reportId: REPORT_ID, data: new DataView(report.buffer) });
      this.dispatchEvent(event);
    }

    // The device goes away (like the restart of the firmware)
    disconnect() {
      this.opened = false;
      // The search and the pairing end with the restart, as the firmware forgets them
      updateRegistered();
      scanStartedMs = null;
      scanStoppedMs = null;
      pairing = null;
      if (registered !== null) {
        registered.sinceMs = performance.now();
        registered.waitState = KEYBOARD_CONNECTING;
        registered.waitMs = RECONNECT_MS;
      }
      const event = new Event("disconnect");
      event.device = this;
      hid.dispatchEvent(event);
    }
  }

  class StubHID extends EventTarget {
    // No chooser: the stub is always chosen
    async requestDevice() {
      return [device];
    }

    async getDevices() {
      return [device];
    }
  }

  const device = new StubDevice();
  const hid = new StubHID();

  Object.defineProperty(Navigator.prototype, "hid", {
    configurable: true,
    enumerable: true,
    get: () => hid,
  });
})();
