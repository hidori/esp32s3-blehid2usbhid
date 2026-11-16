// Connection to the device over WebHID

const unsupportedMessage = document.getElementById("unsupported");
const statusText = document.getElementById("status");
const deviceText = document.getElementById("device");
const deviceMessage = document.getElementById("device-message");
const connectButton = document.getElementById("connect");
const disconnectButton = document.getElementById("disconnect");
const restartButton = document.getElementById("restart");
const keyboardCard = document.getElementById("keyboard-card");
const keyboardMessage = document.getElementById("keyboard-message");
const scanButton = document.getElementById("scan");
const scanResults = document.getElementById("scan-results");
const scanDevicesBody = document.getElementById("scan-devices");
const pairingPanel = document.getElementById("pairing");
const pairingStatus = document.getElementById("pairing-status");
const pairingPasskeyArea = document.getElementById("pairing-passkey-area");
const pairingPasskey = document.getElementById("pairing-passkey");
const pairingCancelButton = document.getElementById("pairing-cancel");

// Devices listed in the chooser: VID and PID of the firmware, with the vendor-defined settings collection
const DEVICE_FILTERS = [{ vendorId: 0x303a, productId: 0x4005, usagePage: 0xff00, usage: 0x01 }];

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
const RESULT_SUCCESS = 0x00;
const RESULT_NAMES = new Map([
  [0x00, "成功"],
  [0x01, "不明なコマンド"],
  [0x02, "引数が不正"],
  [0x03, "保存に失敗"],
  [0x04, "BLE の操作に失敗"],
]);

// Pairing states (see SPEC.md)
const PAIRING_IDLE = 0;
const PAIRING_CONNECTING = 1;
const PAIRING_PAIRING = 2;
const PAIRING_PASSKEY = 3;
const PAIRING_REGISTERED = 4;
const PAIRING_CONNECT_FAILED = 5;
const PAIRING_PAIR_FAILED = 6;
const PAIRING_SAVE_FAILED = 7;

// Time to wait for a response from the device
const RESPONSE_TIMEOUT_MS = 1000;

// Interval of reading the search result while searching, and the pairing state while pairing
const POLL_INTERVAL_MS = 500;

// The opened HID device (null while not connected)
let device = null;

// Search for BLE keyboards: "idle", "scanning" or "stopping"
let scanState = "idle";
// Changed when the search shown on the page is abandoned, so that its loop stops
let scanSession = 0;

// Rows of the search result table: { tr, nameCell, addressCell, rssiCell, pairButton, found }
const scanRows = [];

// Pairing shown on the page: null while not pairing, or { name, state, passkey, cancelRequested }
let pairing = null;
// Changed when the pairing shown on the page is abandoned, so that its loop stops
let pairingSession = 0;

const utf8 = new TextDecoder();

// Format an 8-bit value as 2 hex digits (e.g. 4C)
function hex2(value) {
  return value.toString(16).toUpperCase().padStart(2, "0");
}

// Format a 16-bit ID as 4 hex digits (e.g. 303A)
function hex4(value) {
  return value.toString(16).toUpperCase().padStart(4, "0");
}

// Show the connection state on the page
function render() {
  const connected = device !== null;
  statusText.textContent = connected ? "接続中" : "未接続";
  statusText.classList.toggle("connected", connected);
  if (connected) {
    const name = device.productName || "（製品名なし）";
    deviceText.textContent = `${name}（${hex4(device.vendorId)}:${hex4(device.productId)}）`;
  } else {
    deviceText.textContent = "";
  }
  connectButton.disabled = connected;
  disconnectButton.disabled = !connected;
  restartButton.disabled = !connected;
  // BLE keyboards can be searched and paired only while connected, and not while pairing (see SPEC.md)
  keyboardCard.hidden = !connected;
  scanButton.textContent = scanState === "idle" ? "検索" : "停止";
  scanButton.disabled = !connected || scanState === "stopping" || pairing !== null;
  for (const row of scanRows) {
    row.pairButton.disabled = !connected || scanState === "stopping" || pairing !== null;
  }
  renderPairing();
}

// Show the pairing in progress
function renderPairing() {
  pairingPanel.hidden = pairing === null;
  if (pairing === null) {
    return;
  }
  const showPasskey = pairing.state === PAIRING_PASSKEY && !pairing.cancelRequested;
  if (pairing.cancelRequested) {
    pairingStatus.textContent = "中止しています…";
  } else if (pairing.state === PAIRING_CONNECTING) {
    pairingStatus.textContent = `${pairing.name} に接続しています…`;
  } else if (showPasskey) {
    pairingStatus.textContent = `${pairing.name} とペアリングしています`;
  } else {
    pairingStatus.textContent = `${pairing.name} とペアリングしています…`;
  }
  pairingPasskeyArea.hidden = !showPasskey;
  pairingPasskey.textContent = showPasskey ? String(pairing.passkey).padStart(6, "0") : "";
  pairingCancelButton.disabled = pairing.cancelRequested;
}

// Show the result of an operation in the message area of its card (in red if it is an error)
function showMessage(area, text, isError = false) {
  area.textContent = text;
  area.classList.toggle("error", isError);
}

// Show an error in the message area of a card
function showError(area, error) {
  showMessage(area, `エラー：${error.message}`, true);
}

// Name of a result code in a response
function resultName(result) {
  return RESULT_NAMES.get(result) ?? `不明な結果（0x${hex2(result)}）`;
}

// Show the result code of a response to an operation in the message area of its card
function showResult(area, operation, result) {
  showMessage(area, `${operation}：${resultName(result)}`, result !== RESULT_SUCCESS);
}

// Send a request and wait for its response (resolves with the response data without the report ID)
function request(command, args = new Uint8Array(0)) {
  const target = device;
  // Disconnected meanwhile (e.g. between the requests of the search)
  if (target === null) {
    return Promise.reject(new Error("デバイスが接続されていません"));
  }
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
    report.set(args, 1);
    target.sendReport(REPORT_ID, report).catch((error) => {
      finish();
      reject(error);
    });
  });
}

// Wait for the time
function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Format an address received in little endian as AA:BB:CC:DD:EE:FF
function formatAddress(bytes) {
  return [...bytes].reverse().map(hex2).join(":");
}

// Read the device at the index in the search result (see SPEC.md); found is null if the index is past the end
async function readScanResult(index) {
  const response = await request(COMMAND_GET_SCAN_RESULT, Uint8Array.of(index));
  const result = response.getUint8(1);
  if (result !== RESULT_SUCCESS) {
    throw new Error(`検索結果の取得：${resultName(result)}`);
  }
  const data = new DataView(response.buffer, response.byteOffset + 2, response.byteLength - 2);
  const scanning = data.getUint8(0) !== 0;
  if (index >= data.getUint8(1)) {
    return { scanning, found: null };
  }
  const bytes = (offset, size) => new Uint8Array(data.buffer, data.byteOffset + offset, size);
  const found = {
    addressType: data.getUint8(2),
    addressBytes: bytes(3, 6).slice(),
    address: formatAddress(bytes(3, 6)),
    rssi: data.getInt8(9),
    name: utf8.decode(bytes(11, data.getUint8(10))),
  };
  return { scanning, found };
}

// Read all the devices in the search result
async function readScanResults() {
  const devices = [];
  for (;;) {
    const { scanning, found } = await readScanResult(devices.length);
    if (found === null) {
      return { scanning, devices };
    }
    devices.push(found);
  }
}

// Add a row to the search result table
function addScanRow() {
  const nameCell = document.createElement("td");
  const addressCell = document.createElement("td");
  addressCell.className = "address";
  const rssiCell = document.createElement("td");
  rssiCell.className = "rssi";
  const pairButton = document.createElement("button");
  pairButton.className = "row-action";
  pairButton.type = "button";
  pairButton.textContent = "ペアリング";
  const operationCell = document.createElement("td");
  operationCell.className = "operation";
  operationCell.append(pairButton);
  const tr = document.createElement("tr");
  tr.append(nameCell, addressCell, rssiCell, operationCell);
  scanDevicesBody.append(tr);

  const row = { tr, nameCell, addressCell, rssiCell, pairButton, found: null };
  pairButton.addEventListener("click", () => {
    startPairing(row.found).catch((error) => showError(keyboardMessage, error));
  });
  scanRows.push(row);
  return row;
}

// Show the devices in the search result on the page. The rows are updated in place rather than replaced,
// so that a click on a button is not lost when the result is read again while the button is being pressed
function showScanResults(devices) {
  devices.forEach((found, i) => {
    const row = scanRows[i] ?? addScanRow();
    row.found = found;
    row.nameCell.textContent = found.name || "（名前なし）";
    row.nameCell.classList.toggle("no-name", !found.name);
    row.addressCell.textContent = found.address;
    row.rssiCell.textContent = `${found.rssi} dBm`;
  });
  for (const row of scanRows.splice(devices.length)) {
    row.tr.remove();
  }
  scanResults.hidden = devices.length === 0;
  render();
}

// Remove the search result from the page (with the message about it) and abandon the search shown on the page
function clearScan() {
  scanSession++;
  scanState = "idle";
  showScanResults([]);
  showMessage(keyboardMessage, "");
  render();
}

// Start the search and show the result until the search stops
async function startScan() {
  const response = await request(COMMAND_START_SCAN);
  const result = response.getUint8(1);
  if (result !== RESULT_SUCCESS) {
    showResult(keyboardMessage, "検索", result);
    return;
  }
  clearScan();
  const session = scanSession;
  scanState = "scanning";
  showMessage(keyboardMessage, "検索しています…");
  render();
  try {
    for (;;) {
      const { scanning, devices } = await readScanResults();
      if (session !== scanSession) {
        return;
      }
      showScanResults(devices);
      if (!scanning) {
        showMessage(keyboardMessage, devices.length > 0 ? `${devices.length} 台見つかりました` : "見つかりませんでした");
        break;
      }
      await sleep(POLL_INTERVAL_MS);
    }
  } catch (error) {
    if (session !== scanSession) {
      return;
    }
    showError(keyboardMessage, error);
  }
  scanState = "idle";
  render();
}

// Stop the search (the loop of startScan() shows the result when it sees that the search has stopped)
async function stopScan() {
  scanState = "stopping";
  render();
  try {
    const response = await request(COMMAND_STOP_SCAN);
    const result = response.getUint8(1);
    if (result !== RESULT_SUCCESS) {
      throw new Error(`停止：${resultName(result)}`);
    }
  } catch (error) {
    if (scanState === "stopping") {
      scanState = "scanning";
      render();
    }
    throw error;
  }
}

// Read the pairing state (see SPEC.md)
async function readPairingState() {
  const response = await request(COMMAND_GET_PAIRING_STATE);
  const result = response.getUint8(1);
  if (result !== RESULT_SUCCESS) {
    throw new Error(`ペアリングの状態の取得：${resultName(result)}`);
  }
  return { state: response.getUint8(2), passkey: response.getUint32(3, true) };
}

// Whether the pairing state is one while pairing (see SPEC.md)
function isPairingState(state) {
  return state === PAIRING_CONNECTING || state === PAIRING_PAIRING || state === PAIRING_PASSKEY;
}

// Show how the pairing with the named device ended
function showPairingResult(name, state) {
  switch (state) {
    case PAIRING_IDLE:
      showMessage(keyboardMessage, "ペアリングを中止しました");
      break;
    case PAIRING_REGISTERED:
      showMessage(keyboardMessage, `${name} を登録しました`);
      break;
    case PAIRING_CONNECT_FAILED:
      showMessage(
        keyboardMessage,
        `${name} に接続できませんでした。BLE キーボードがペアリングできる状態か確かめてください`,
        true,
      );
      break;
    case PAIRING_PAIR_FAILED:
      showMessage(keyboardMessage, `${name} とペアリングできませんでした（パスキーの入力の誤り、時間切れなど）`, true);
      break;
    case PAIRING_SAVE_FAILED:
      showMessage(keyboardMessage, `${name} とペアリングできましたが、登録を保存できませんでした`, true);
      break;
    default:
      showMessage(keyboardMessage, `ペアリング：不明な状態（0x${hex2(state)}）`, true);
      break;
  }
}

// Pair with a device in the search result and show the progress until the pairing ends
async function startPairing(found) {
  const args = Uint8Array.of(found.addressType, ...found.addressBytes);
  const response = await request(COMMAND_START_PAIRING, args);
  const result = response.getUint8(1);
  if (result !== RESULT_SUCCESS) {
    showResult(keyboardMessage, "ペアリング", result);
    return;
  }
  // The device has stopped the search to connect: stop showing it, keeping the list
  scanSession++;
  scanState = "idle";
  const session = ++pairingSession;
  const name = found.name || found.address;
  pairing = { name, state: PAIRING_CONNECTING, passkey: 0, cancelRequested: false };
  showMessage(keyboardMessage, "");
  render();
  try {
    for (;;) {
      const { state, passkey } = await readPairingState();
      if (session !== pairingSession) {
        return;
      }
      if (!isPairingState(state)) {
        showPairingResult(name, state);
        break;
      }
      Object.assign(pairing, { state, passkey });
      render();
      await sleep(POLL_INTERVAL_MS);
    }
  } catch (error) {
    if (session !== pairingSession) {
      return;
    }
    showError(keyboardMessage, error);
  }
  pairing = null;
  render();
}

// Cancel the pairing (the loop of startPairing() shows the result when it sees that the pairing has ended)
async function cancelPairing() {
  pairing.cancelRequested = true;
  render();
  try {
    const response = await request(COMMAND_CANCEL_PAIRING);
    const result = response.getUint8(1);
    if (result !== RESULT_SUCCESS) {
      throw new Error(`中止：${resultName(result)}`);
    }
  } catch (error) {
    if (pairing !== null) {
      pairing.cancelRequested = false;
      render();
    }
    throw error;
  }
}

// Abandon the pairing shown on the page
function clearPairing() {
  pairingSession++;
  pairing = null;
  render();
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
  showMessage(deviceMessage, "");
  render();
}

// Close the opened device
async function disconnect() {
  if (device?.opened) {
    await device.close();
  }
  device = null;
  showMessage(deviceMessage, "");
  clearPairing();
  clearScan();
}

// Restart the device (the device is disconnected after the response)
async function restart() {
  const response = await request(COMMAND_RESTART);
  showResult(deviceMessage, "再起動", response.getUint8(1));
}

if ("hid" in navigator) {
  connectButton.addEventListener("click", () => connect().catch((error) => showError(deviceMessage, error)));
  disconnectButton.addEventListener("click", () => disconnect().catch((error) => showError(deviceMessage, error)));
  restartButton.addEventListener("click", () => restart().catch((error) => showError(deviceMessage, error)));
  scanButton.addEventListener("click", () => {
    const operation = scanState === "idle" ? startScan() : stopScan();
    operation.catch((error) => showError(keyboardMessage, error));
  });
  pairingCancelButton.addEventListener("click", () => {
    cancelPairing().catch((error) => showError(keyboardMessage, error));
  });
  // The device was unplugged
  navigator.hid.addEventListener("disconnect", (event) => {
    if (event.device === device) {
      device = null;
      clearPairing();
      clearScan();
    }
  });
  render();
} else {
  unsupportedMessage.hidden = false;
  connectButton.disabled = true;
  disconnectButton.disabled = true;
  restartButton.disabled = true;
}
