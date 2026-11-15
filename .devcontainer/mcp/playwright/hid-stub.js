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
  const COMMAND_GET_MACRO = 0x02;
  const COMMAND_SET_MACRO = 0x03;
  const RESULT_SUCCESS = 0x00;
  const RESULT_UNKNOWN_COMMAND = 0x01;

  // Time until a response arrives, and until the device is disconnected by a restart
  const RESPONSE_DELAY_MS = 10;
  const RESTART_DELAY_MS = 100;

  // Macro kept until the page is reloaded (it survives a restart like the NVS).
  // At first, the example in SPEC.md: DEL, F1 and F2, 100 ms each, 100 ms between the keys
  let macro = new Uint8Array([
    100, 0, 3,
    0x00, 0x4c, 100, 0,
    0x00, 0x3a, 100, 0,
    0x00, 0x3b, 100, 0,
  ]);

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
        case COMMAND_GET_MACRO:
          this.respond(command, RESULT_SUCCESS, macro);
          break;
        case COMMAND_SET_MACRO: {
          const count = request[1 + 2];
          macro = request.slice(1, 1 + 3 + 4 * count);
          this.respond(command, RESULT_SUCCESS);
          break;
        }
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
