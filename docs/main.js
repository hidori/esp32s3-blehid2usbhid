// Connection to the device over WebHID

const unsupportedMessage = document.getElementById("unsupported");
const statusText = document.getElementById("status");
const messageText = document.getElementById("message");
const connectButton = document.getElementById("connect");
const disconnectButton = document.getElementById("disconnect");
const restartButton = document.getElementById("restart");

// Devices listed in the chooser: VID and PID of the firmware, with the vendor-defined settings collection
const DEVICE_FILTERS = [{ vendorId: 0x303a, productId: 0x4005, usagePage: 0xff00, usage: 0x01 }];

// Settings HID protocol (see SPEC.md)
const REPORT_ID = 2;
const REPORT_SIZE = 63;
const COMMAND_RESTART = 0x01;
const RESULT_NAMES = new Map([
  [0x00, "成功"],
  [0x01, "不明なコマンド"],
]);

// Time to wait for a response from the device
const RESPONSE_TIMEOUT_MS = 1000;

// The opened HID device (null while not connected)
let device = null;

// Format a 16-bit ID as 4 hex digits (e.g. 303A)
function hex4(value) {
  return value.toString(16).toUpperCase().padStart(4, "0");
}

// Show the connection state on the page
function render() {
  if (device) {
    const name = device.productName || "（製品名なし）";
    statusText.textContent = `接続中：${name}（${hex4(device.vendorId)}:${hex4(device.productId)}）`;
  } else {
    statusText.textContent = "未接続";
  }
  connectButton.disabled = device !== null;
  disconnectButton.disabled = device === null;
  restartButton.disabled = device === null;
}

// Show the result of an operation on the page
function showMessage(text) {
  messageText.textContent = text;
}

// Show an error on the page
function showError(error) {
  showMessage(`エラー：${error.message}`);
}

// Name of a result code in a response
function resultName(result) {
  return RESULT_NAMES.get(result) ?? `不明な結果（0x${result.toString(16).toUpperCase().padStart(2, "0")}）`;
}

// Send a request and wait for its response (resolves with the response data without the report ID)
function request(command) {
  const target = device;
  return new Promise((resolve, reject) => {
    let timer;
    const finish = () => {
      clearTimeout(timer);
      target.removeEventListener("inputreport", onInputReport);
    };
    const onInputReport = (event) => {
      if (event.reportId !== REPORT_ID || event.data.byteLength < 2 || event.data.getUint8(0) !== command) {
        return; // Not the response to this request
      }
      finish();
      resolve(event.data);
    };
    timer = setTimeout(() => {
      finish();
      reject(new Error("デバイスから応答がありません"));
    }, RESPONSE_TIMEOUT_MS);
    target.addEventListener("inputreport", onInputReport);

    const report = new Uint8Array(REPORT_SIZE);
    report[0] = command;
    target.sendReport(REPORT_ID, report).catch((error) => {
      finish();
      reject(error);
    });
  });
}

// Let the user choose a device and open it
async function connect() {
  const [selected] = await navigator.hid.requestDevice({ filters: DEVICE_FILTERS });
  if (!selected) {
    return; // The chooser was canceled
  }
  if (!selected.opened) {
    await selected.open();
  }
  device = selected;
  render();
}

// Close the opened device
async function disconnect() {
  if (device?.opened) {
    await device.close();
  }
  device = null;
  render();
}

// Restart the device (the device is disconnected after the response)
async function restart() {
  const response = await request(COMMAND_RESTART);
  showMessage(`再起動：${resultName(response.getUint8(1))}`);
}

if ("hid" in navigator) {
  connectButton.addEventListener("click", () => connect().catch(showError));
  disconnectButton.addEventListener("click", () => disconnect().catch(showError));
  restartButton.addEventListener("click", () => restart().catch(showError));
  // The device was unplugged
  navigator.hid.addEventListener("disconnect", (event) => {
    if (event.device === device) {
      device = null;
      render();
    }
  });
  render();
} else {
  unsupportedMessage.hidden = false;
  connectButton.disabled = true;
  disconnectButton.disabled = true;
  restartButton.disabled = true;
}
