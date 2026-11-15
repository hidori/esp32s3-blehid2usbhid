// Connection to the device over WebHID

import { KEY_USAGES } from "./key-usages.js";

const unsupportedMessage = document.getElementById("unsupported");
const statusText = document.getElementById("status");
const messageText = document.getElementById("message");
const connectButton = document.getElementById("connect");
const disconnectButton = document.getElementById("disconnect");
const restartButton = document.getElementById("restart");
const macroGapInput = document.getElementById("macro-gap");
const macroKeysBody = document.getElementById("macro-keys");
const macroAddButton = document.getElementById("macro-add");
const macroSaveButton = document.getElementById("macro-save");

// Devices listed in the chooser: VID and PID of the firmware, with the vendor-defined settings collection
const DEVICE_FILTERS = [{ vendorId: 0x303a, productId: 0x4005, usagePage: 0xff00, usage: 0x01 }];

// Settings HID protocol (see SPEC.md)
const REPORT_ID = 2;
const REPORT_SIZE = 63;
const COMMAND_RESTART = 0x01;
const COMMAND_GET_MACRO = 0x02;
const COMMAND_SET_MACRO = 0x03;
const RESULT_SUCCESS = 0x00;
const RESULT_NAMES = new Map([
  [0x00, "成功"],
  [0x01, "不明なコマンド"],
  [0x02, "引数が不正"],
  [0x03, "保存に失敗"],
]);

// Macro (see SPEC.md)
const MACRO_MAX_KEYS = 10;
const MACRO_MAX_TIME_MS = 5000;
const MACRO_DEFAULT_PRESS_MS = 100;

// Modifier keys in the order of the bits in a macro (see SPEC.md)
const MODIFIERS = [
  { label: "左Ctrl", code: "ControlLeft" },
  { label: "左Shift", code: "ShiftLeft" },
  { label: "左Alt", code: "AltLeft" },
  { label: "左GUI", code: "MetaLeft" },
  { label: "右Ctrl", code: "ControlRight" },
  { label: "右Shift", code: "ShiftRight" },
  { label: "右Alt", code: "AltRight" },
  { label: "右GUI", code: "MetaRight" },
];

// KeyboardEvent.code value of each HID usage ID, used as the name of the key
const KEY_NAMES = new Map([...KEY_USAGES].map(([code, usage]) => [usage, code]));

// Time to wait for a response from the device
const RESPONSE_TIMEOUT_MS = 1000;

// The opened HID device (null while not connected)
let device = null;

// Rows of the macro table: { tr, numberCell, modifiers, keyName, keyCode, press }
const macroRows = [];

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
  if (device) {
    const name = device.productName || "（製品名なし）";
    statusText.textContent = `接続中：${name}（${hex4(device.vendorId)}:${hex4(device.productId)}）`;
  } else {
    statusText.textContent = "未接続";
  }
  const connected = device !== null;
  connectButton.disabled = connected;
  disconnectButton.disabled = !connected;
  restartButton.disabled = !connected;
  macroGapInput.disabled = !connected;
  macroAddButton.disabled = !connected || macroRows.length >= MACRO_MAX_KEYS;
  macroSaveButton.disabled = !connected;
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
  return RESULT_NAMES.get(result) ?? `不明な結果（0x${hex2(result)}）`;
}

// Send a request and wait for its response (resolves with the response data without the report ID)
function request(command, args = new Uint8Array(0)) {
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
    report.set(args, 1);
    target.sendReport(REPORT_ID, report).catch((error) => {
      finish();
      reject(error);
    });
  });
}

// Whether the usage ID can be used as a key in a macro (see SPEC.md)
function isValidKey(usage) {
  return (usage >= 0x04 && usage <= 0xa4) || (usage >= 0xb0 && usage <= 0xdd);
}

// Parse a key code typed in hex ("4C" or "0x4C"); null if it is not a valid key
function parseKey(text) {
  const match = /^(?:0x)?([0-9a-f]{1,2})$/i.exec(text.trim());
  if (!match) {
    return null;
  }
  const usage = parseInt(match[1], 16);
  return isValidKey(usage) ? usage : null;
}

// Parse a time in ms typed on the page; throws if it is out of range
function parseTime(text, what) {
  const value = Number(text);
  if (text.trim() === "" || !Number.isInteger(value) || value < 0 || value > MACRO_MAX_TIME_MS) {
    throw new Error(`${what}は 0〜${MACRO_MAX_TIME_MS} の整数で入力してください`);
  }
  return value;
}

// Show the name of the key whose code is in the row
function showKeyName(row) {
  const usage = parseKey(row.keyCode.value);
  row.keyName.value = usage === null ? "" : (KEY_NAMES.get(usage) ?? "（名前なし）");
}

// Number the rows of the macro table
function numberMacroRows() {
  macroRows.forEach((row, i) => {
    row.numberCell.textContent = String(i + 1);
  });
}

// Let the user specify the key of the row (and the modifier keys held with it) by pressing it
function listenKeyPress(row) {
  const held = new Set();
  const modifierBit = (event) => MODIFIERS.findIndex((modifier) => modifier.code === event.code);
  row.keyName.addEventListener("keydown", (event) => {
    event.preventDefault();
    const bit = modifierBit(event);
    if (bit >= 0) {
      held.add(bit);
      return;
    }
    const usage = KEY_USAGES.get(event.code);
    if (usage === undefined) {
      showMessage(`このキーは指定できません（${event.code || event.key}）`);
      return;
    }
    row.keyCode.value = hex2(usage);
    showKeyName(row);
    row.modifiers.forEach((box, i) => {
      box.checked = held.has(i);
    });
  });
  row.keyName.addEventListener("keyup", (event) => {
    held.delete(modifierBit(event));
  });
  row.keyName.addEventListener("blur", () => {
    held.clear();
  });
}

// Add a row to the macro table; entry is { modifiers, key (null if not set), pressMs }
function addMacroRow(entry) {
  const tr = document.createElement("tr");
  const numberCell = document.createElement("td");

  const modifierCell = document.createElement("td");
  const modifiers = MODIFIERS.map((modifier, bit) => {
    const label = document.createElement("label");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = (entry.modifiers & (1 << bit)) !== 0;
    label.append(box, modifier.label);
    modifierCell.append(label, " ");
    return box;
  });

  const keyName = document.createElement("input");
  keyName.readOnly = true;
  keyName.placeholder = "ここでキーを押す";

  const keyCode = document.createElement("input");
  keyCode.size = 4;
  keyCode.placeholder = "4C";
  keyCode.value = entry.key === null ? "" : hex2(entry.key);

  const press = document.createElement("input");
  press.type = "number";
  press.min = "0";
  press.max = String(MACRO_MAX_TIME_MS);
  press.step = "1";
  press.value = String(entry.pressMs);

  const removeButton = document.createElement("button");
  removeButton.type = "button";
  removeButton.textContent = "削除";

  const cells = [keyName, keyCode, press, removeButton].map((element) => {
    const cell = document.createElement("td");
    cell.append(element);
    return cell;
  });
  tr.append(numberCell, modifierCell, ...cells);

  const row = { tr, numberCell, modifiers, keyName, keyCode, press };
  macroRows.push(row);
  macroKeysBody.append(tr);

  showKeyName(row);
  listenKeyPress(row);
  keyCode.addEventListener("input", () => showKeyName(row));
  removeButton.addEventListener("click", () => {
    macroRows.splice(macroRows.indexOf(row), 1);
    tr.remove();
    numberMacroRows();
    render();
  });

  numberMacroRows();
  render();
  return row;
}

// Remove the macro from the page
function clearMacro() {
  macroRows.length = 0;
  macroKeysBody.replaceChildren();
  macroGapInput.value = "";
  render();
}

// Show the macro data received from the device (see SPEC.md)
function showMacro(data) {
  clearMacro();
  macroGapInput.value = String(data.getUint16(0, true));
  const count = data.getUint8(2);
  for (let i = 0; i < count; i++) {
    const offset = 3 + 4 * i;
    addMacroRow({
      modifiers: data.getUint8(offset),
      key: data.getUint8(offset + 1),
      pressMs: data.getUint16(offset + 2, true),
    });
  }
}

// Build the macro data from the page (see SPEC.md); throws if an input is invalid
function readMacro() {
  const data = new DataView(new ArrayBuffer(3 + 4 * macroRows.length));
  data.setUint16(0, parseTime(macroGapInput.value, "キーとキーの間の時間"), true);
  data.setUint8(2, macroRows.length);
  macroRows.forEach((row, i) => {
    const key = parseKey(row.keyCode.value);
    if (key === null) {
      throw new Error(
        row.keyCode.value.trim() === ""
          ? `${i + 1} 件目のキーを指定してください`
          : `${i + 1} 件目のコードが範囲外です（0x04〜0xA4、0xB0〜0xDD）`,
      );
    }
    const modifiers = row.modifiers.reduce((bits, box, bit) => (box.checked ? bits | (1 << bit) : bits), 0);
    const offset = 3 + 4 * i;
    data.setUint8(offset, modifiers);
    data.setUint8(offset + 1, key);
    data.setUint16(offset + 2, parseTime(row.press.value, `${i + 1} 件目の押している時間`), true);
  });
  return new Uint8Array(data.buffer);
}

// Read the macro from the device and show it
async function loadMacro() {
  const response = await request(COMMAND_GET_MACRO);
  const result = response.getUint8(1);
  if (result !== RESULT_SUCCESS) {
    throw new Error(`マクロの取得：${resultName(result)}`);
  }
  showMacro(new DataView(response.buffer, response.byteOffset + 2, response.byteLength - 2));
}

// Save the macro on the page to the device
async function saveMacro() {
  const macro = readMacro();
  const response = await request(COMMAND_SET_MACRO, macro);
  showMessage(`保存：${resultName(response.getUint8(1))}`);
}

// Let the user choose a device, open it and read the macro
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
  await loadMacro();
}

// Close the opened device
async function disconnect() {
  if (device?.opened) {
    await device.close();
  }
  device = null;
  clearMacro();
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
  macroAddButton.addEventListener("click", () => {
    addMacroRow({ modifiers: 0, key: null, pressMs: MACRO_DEFAULT_PRESS_MS }).keyName.focus();
  });
  macroSaveButton.addEventListener("click", () => saveMacro().catch(showError));
  // The device was unplugged
  navigator.hid.addEventListener("disconnect", (event) => {
    if (event.device === device) {
      device = null;
      clearMacro();
    }
  });
  render();
} else {
  unsupportedMessage.hidden = false;
  connectButton.disabled = true;
  disconnectButton.disabled = true;
  restartButton.disabled = true;
}
