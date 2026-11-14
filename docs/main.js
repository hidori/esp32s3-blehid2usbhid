// Connection to the device over WebHID

const unsupportedMessage = document.getElementById("unsupported");
const statusText = document.getElementById("status");
const connectButton = document.getElementById("connect");
const disconnectButton = document.getElementById("disconnect");

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
}

// Show an error on the page
function showError(error) {
  statusText.textContent = `エラー：${error.message}`;
}

// Let the user choose a device and open it
async function connect() {
  // No filters for now: every HID device is listed in the chooser
  const [selected] = await navigator.hid.requestDevice({ filters: [] });
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

if ("hid" in navigator) {
  connectButton.addEventListener("click", () => connect().catch(showError));
  disconnectButton.addEventListener("click", () => disconnect().catch(showError));
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
}
