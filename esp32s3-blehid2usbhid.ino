// BLE HID to USB HID bridge

#include <Adafruit_TinyUSB.h>
#include <EspBle.h>
#include <Preferences.h>
#include <host/ble_hs.h>
#include <esp_mac.h>
#include <nvs_flash.h>

// Interval of loop()
const uint32_t LOOP_INTERVAL_MS = 1;

// Settings HID protocol (see SPEC.md)
const uint8_t SETTINGS_REPORT_ID = 2;
const uint8_t SETTINGS_REPORT_SIZE = 63;
const uint8_t COMMAND_RESTART = 0x01;
const uint8_t COMMAND_START_SCAN = 0x04;
const uint8_t COMMAND_STOP_SCAN = 0x05;
const uint8_t COMMAND_GET_SCAN_RESULT = 0x06;
const uint8_t COMMAND_START_PAIRING = 0x07;
const uint8_t COMMAND_CANCEL_PAIRING = 0x08;
const uint8_t COMMAND_GET_PAIRING_STATE = 0x09;
const uint8_t COMMAND_GET_REGISTRATION = 0x0A;
const uint8_t RESULT_SUCCESS = 0x00;
const uint8_t RESULT_UNKNOWN_COMMAND = 0x01;
const uint8_t RESULT_INVALID_ARGUMENT = 0x02;
const uint8_t RESULT_BLE_FAILED = 0x04;

// Search for BLE keyboards and the scan result data (see SPEC.md)
const uint8_t SCAN_MAX_DEVICES = 16;
const size_t SCAN_NAME_MAX_SIZE = 29;  // Longest name an advertising payload (31 bytes) can carry
const size_t SCAN_RESULT_HEADER_SIZE = 11;
static_assert(SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "a scan result must fit in a response");
const uint16_t APPEARANCE_GENERIC_HID = 0x03C0;
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;

// HID over GATT: the HID service, its Report Map and Reports, the descriptors of a Report (Report Reference, and the
// Client Characteristic Configuration to subscribe), and the type of an input report in the Report Reference
const char UUID_HID_SERVICE[] = "1812";
const char UUID_REPORT_MAP[] = "2a4b";
const char UUID_REPORT[] = "2a4d";
const char UUID_REPORT_REFERENCE[] = "2908";
const char UUID_CCCD[] = "2902";
const uint16_t UUID16_REPORT_REFERENCE = 0x2908;
const uint16_t UUID16_CCCD = 0x2902;
const uint8_t REPORT_TYPE_INPUT = 1;
const uint8_t REPORT_TYPE_OUTPUT = 2;
const size_t HID_MAX_REPORTS = 16;       // Reports of the HID service that are looked at
const size_t REPORT_MAP_MAX_SIZE = 512;  // Longest attribute value (Bluetooth Core Specification)

// Notifications and results of the GATT operations made with NimBLE directly, passed from the NimBLE host task to
// loop(). A longer notification is not relayed (a keyboard input report is far shorter)
const size_t NIMBLE_EVENT_QUEUE_SIZE = 32;
const size_t NIMBLE_EVENT_MAX_DATA = 64;

// Pairing state data (see SPEC.md)
const size_t PAIRING_STATE_SIZE = 5;

// Registration data (see SPEC.md)
const size_t REGISTRATION_HEADER_SIZE = 10;
static_assert(REGISTRATION_HEADER_SIZE + SCAN_NAME_MAX_SIZE <= SETTINGS_REPORT_SIZE - 2, "the registration must fit in a response");

// Most bonds (pairing information) the NimBLE host keeps in the NVS
const size_t BOND_CAPACITY = CONFIG_BT_NIMBLE_MAX_BONDS;

// Length of an attempt to connect to the registered keyboard. The attempts follow one another, so this does not delay
// the connection; it only bounds how long the start of a search waits for an attempt to end
const uint32_t KEYBOARD_CONNECT_ATTEMPT_MS = 1000;

// Connection interval requested while the HID service of the registered keyboard is prepared, in units of 1.25 ms
// (7.5-15 ms). Each GATT operation takes round trips over the connection, so a short interval shortens the
// preparation. The parameters of the connection are put back when the relay starts
const uint16_t PREPARATION_INTERVAL_MIN = 6;
const uint16_t PREPARATION_INTERVAL_MAX = 12;

// Usages of the HID Keyboard/Keypad page: the errors (ErrorRollOver, POSTFail, ErrorUndefined), the keys, and the
// modifiers 0xE0-0xE7, which go in the modifier byte
const uint16_t USAGE_PAGE_KEYBOARD = 0x07;
const uint8_t USAGE_ERROR_ROLL_OVER = 0x01;
const uint8_t USAGE_ERROR_UNDEFINED = 0x03;
const uint8_t USAGE_FIRST_KEY = 0x04;
const uint8_t USAGE_LAST_KEY = 0xDF;
const uint8_t USAGE_FIRST_MODIFIER = 0xE0;

// Usages of the HID Consumer page (media keys): those that the consumer control collection sends (see SPEC.md)
const uint16_t USAGE_PAGE_CONSUMER = 0x0C;
const uint16_t USAGE_LAST_CONSUMER = 0x3FF;

// Usages of the HID LED page (lamps): those that the host sends to a boot keyboard, Num Lock (bit 0) to Kana (bit 4)
// (see SPEC.md)
const uint16_t USAGE_PAGE_LED = 0x08;
const uint16_t USAGE_FIRST_LAMP = 0x01;
const uint16_t USAGE_LAST_LAMP = 0x05;

// Application collections with the keys and the lamps, as a usage with its page in the upper half: a keyboard (Generic
// Desktop page, Keyboard) and a consumer control (Consumer page, Consumer Control)
const uint32_t COLLECTION_KEYBOARD = 0x00010006;
const uint32_t COLLECTION_CONSUMER_CONTROL = 0x000C0001;

// Main items of a Report Map whose fields are read: those of the input reports or of the output reports
const uint8_t ITEM_INPUT = 8;
const uint8_t ITEM_OUTPUT = 9;

// Keys in a boot keyboard report, and the reports waiting to be sent to the host
const size_t KEYBOARD_REPORT_KEYS = 6;
const size_t KEYBOARD_REPORT_QUEUE_SIZE = 16;

// Media key reports waiting to be sent to the host, and the media keys pressed at once that are tracked
const size_t CONSUMER_REPORT_QUEUE_SIZE = 16;
const size_t CONSUMER_MAX_PRESSED = 16;

// Layout of the keyboard input reports, the media key reports and the lamp output reports of the BLE keyboard, read from
// its Report Map
const size_t FIELD_MAX_USAGES = 16;     // Usages listed one by one for a field
const size_t FORMAT_MAX_FIELDS = 4;     // Fields with keys or lamps in a report
const size_t KEYBOARD_MAX_FORMATS = 4;  // Keyboard input reports (report IDs)
const size_t CONSUMER_MAX_FORMATS = 4;  // Media key reports (report IDs)
const size_t LAMP_MAX_FORMATS = 4;      // Lamp output reports (report IDs)

// Longest lamp output report written: the value of a Write Without Response that fits the smallest ATT MTU (23 bytes)
const size_t LAMP_REPORT_MAX_SIZE = 20;

// NVS (the ID marks that the NVS has been initialized by this firmware)
const char NVS_NAMESPACE[] = "settings";
const char NVS_KEY_ID[] = "id";
const char NVS_KEY_KEYBOARD[] = "keyboard";   // Address and name of the registered keyboard
const char NVS_KEY_HID_CACHE[] = "hidcache";  // The HID service of the registered keyboard, remembered
const uint32_t NVS_ID = 0xB1E2B5B0;
const uint8_t HID_CACHE_VERSION = 3;  // Layout of the remembered HID service; change it when the layout changes

// Keyboard (boot protocol, no report ID so that boot and report protocol share the format)
uint8_t const descKeyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

// Consumer control for the media keys: one usage of 0x000-0x3FF at a time, as the sample of Microsoft (see SPEC.md)
const uint8_t CONSUMER_REPORT_ID = 1;

// Consumer control and vendor-defined settings collection (input and output reports)
uint8_t const descSettings[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(CONSUMER_REPORT_ID)),
    TUD_HID_REPORT_DESC_GENERIC_INOUT(SETTINGS_REPORT_SIZE, HID_REPORT_ID(SETTINGS_REPORT_ID)),
};

Adafruit_USBD_HID keyboard;
Adafruit_USBD_HID settings;

EspBle ble;

// USB product name with the last 2 bytes of the factory MAC address as the device ID
static char productName[40];

// Lamps (Caps Lock etc.) that the host has, received in the USB task, as in a boot keyboard output report: Num Lock
// (bit 0) to Kana (bit 4)
static volatile uint8_t hostLamps;

// Requests from the settings page, passed from the USB task to loop()
static QueueHandle_t settingsRequests;

// BLE keyboards found by the search
struct ScannedDevice {
  uint8_t addressType;  // As in SPEC.md
  uint8_t address[6];   // Little endian
  int8_t rssi;
  uint8_t nameSize;
  char name[SCAN_NAME_MAX_SIZE];  // UTF-8, not terminated
};
static ScannedDevice scannedDevices[SCAN_MAX_DEVICES];
static uint8_t scannedCount;

// Pairing with a BLE keyboard (the values are those of SPEC.md)
enum PairingState : uint8_t {
  PAIRING_IDLE = 0,                 // Not pairing
  PAIRING_CONNECTING = 1,           // Connecting (also while preparing to connect)
  PAIRING_PAIRING = 2,              // Pairing (also while the registration is replaced after the pairing)
  PAIRING_PASSKEY = 3,              // Waiting for the passkey to be typed on the keyboard
  PAIRING_REGISTERED = 4,           // Registered
  PAIRING_CONNECT_FAILED = 5,       // Failed to connect
  PAIRING_PAIR_FAILED = 6,          // Failed to pair
  PAIRING_REGISTRATION_FAILED = 7,  // Failed to rewrite the registration
  PAIRING_NOT_KEYBOARD = 8,         // Not registered, since the device has no keyboard input
};
static PairingState pairingState = PAIRING_IDLE;
static bool pairingPreparing;                         // Waiting for the relay to stop before connecting
static EspBleBond pairingTarget;                      // The device to pair with
static uint8_t pairingTargetNameSize;                 // Name of the device in the scan result
static char pairingTargetName[SCAN_NAME_MAX_SIZE];
static uint32_t pairingPasskey;                       // Valid in PAIRING_PASSKEY
static EspBleConnectionId pairingConnectionId;        // 0 while not connected
static EspBleConnection pairingConnection;            // As it was when bonded, handed over to the relay
static bool pairingShortInterval;                     // A short interval has been requested to find the HID service
static bool pairingCancelRequested;                   // Canceled; ends when the connection is closed
static bool pairingBonded;                            // Made a bond; registered before the relay or after closing
static bool pairingNotKeyboard;                       // The HID service shows no keyboard input: not registered
static EspBleBond bondsBeforePairing[BOND_CAPACITY];  // Bonds of the other devices when the pairing started
static size_t bondsBeforePairingCount;

// Name of the registered keyboard, kept in the NVS with its address, since a bond has no name. The name is valid only
// while the address is that of the registered keyboard (see SPEC.md)
struct KeyboardName {
  uint8_t addressType;  // As in SPEC.md
  uint8_t address[6];   // Little endian
  uint8_t nameSize;
  char name[SCAN_NAME_MAX_SIZE];  // UTF-8, not terminated
};
static KeyboardName keyboardName;
static const size_t KEYBOARD_NAME_HEADER_SIZE = offsetof(KeyboardName, name);

// Search requested while an attempt to connect to the registered keyboard is in progress (started when it ends)
static bool scanRequested;

// Connection to the registered keyboard, whose input is relayed to the host (see SPEC.md)
static bool keyboardConnecting;                  // An attempt to connect is in progress
static EspBleConnectionId keyboardConnectionId;  // 0 while not connected
static bool keyboardReady;  // Subscribed to the keyboard input after the encryption with the bond: relaying
static EspBleBond keyboardTarget;                // The registered keyboard that the attempt connects to
static uint16_t keyboardConnectionHandle;        // NimBLE handle of the connection
static bool keyboardUsesCache;                   // Uses the remembered HID service instead of discovering it
static bool keyboardShortInterval;               // A short interval has been requested for the preparation
// Parameters of the connection when it was made (units of EspBleConnection), put back when the relay starts
static uint16_t keyboardInterval;
static uint16_t keyboardLatency;
static uint16_t keyboardSupervisionTimeout;

// State of the connection to the registered keyboard (the values are those of SPEC.md)
enum KeyboardState : uint8_t {
  KEYBOARD_IDLE = 0,        // Not connecting (including while searching or pairing)
  KEYBOARD_CONNECTING = 1,  // Trying to connect
  KEYBOARD_PREPARING = 2,   // Connected, encrypting and preparing the HID service
  KEYBOARD_RELAYING = 3,    // Relaying the input
};

// Reports waiting to be sent to the host. Notifications from the keyboard can arrive back to back, before the host has
// taken the previous report; dropping one would lose a key or leave a key pressed. A report is queued unless it is the
// state the host will already have. When the queue is full, the last report is replaced, so that the host still ends
// up in the latest state
template <typename Report, size_t SIZE>
struct ReportQueue {
  Report reports[SIZE];
  size_t head;
  size_t count;
  Report last;  // What the host will have when the queue has been sent

  void push(const Report &report) {
    if (memcmp(&report, &last, sizeof(report)) == 0) {
      return;
    }
    last = report;
    if (count == SIZE) {
      reports[(head + count - 1) % SIZE] = report;
      return;
    }
    reports[(head + count) % SIZE] = report;
    count++;
  }

  const Report &front() const { return reports[head]; }

  void pop() {
    head = (head + 1) % SIZE;
    count--;
  }
};

// Boot keyboard report, and the reports waiting to be sent to the host
struct KeyboardReport {
  uint8_t modifiers;
  uint8_t keys[KEYBOARD_REPORT_KEYS];
};
static ReportQueue<KeyboardReport, KEYBOARD_REPORT_QUEUE_SIZE> keyboardQueue;

// Media key reports (the usage of the media key pressed, 0 for none) waiting to be sent to the host
static ReportQueue<uint16_t, CONSUMER_REPORT_QUEUE_SIZE> consumerQueue;

// Keys or lamps in a field of a report: each element holds the index of a pressed key or a lit lamp (array), or the
// state of one key or lamp (variable)
struct UsageField {
  uint16_t bitOffset;      // Of the first element, in the report without the report ID
  uint8_t bitSize;         // Of an element (1 to 32)
  uint16_t count;          // Elements
  bool array;
  int32_t logicalMinimum;  // Array: the value of the first usage
  int32_t logicalMaximum;
  uint16_t usageMinimum;   // Usages on the page of the field: this range, or the list when usageCount is not 0
  uint16_t usageMaximum;
  uint8_t usageCount;
  bool usagesCut;          // More usages are listed than kept: the elements past those kept have no usage
  uint16_t usages[FIELD_MAX_USAGES];
};

// Layout of the keys in a keyboard input report or a media key report, or of the lamps in a lamp output report
struct UsageFormat {
  uint8_t reportId;    // 0 when the Report Map has no report IDs
  uint16_t bitLength;  // Of the whole report, without the report ID
  uint8_t fieldCount;
  UsageField fields[FORMAT_MAX_FIELDS];
};

// Called with the usage of each key pressed in a report, in the order of the report
typedef void (*PressedKeyHandler)(uint16_t usage, void *context);

// Media keys pressed in a media key report, in the order of the report
struct PressedConsumerKeys {
  uint8_t count;
  uint16_t usages[CONSUMER_MAX_PRESSED];
};

// HID service of a keyboard, prepared one GATT operation at a time after the encryption (EspBle queues at most 8
// operations): discovered with EspBle on the first connection, and checked with NimBLE directly when remembered, since
// any GATT operation of EspBle discovers the whole database first. The keyboard input reports are prepared first, and
// the media key reports and the lamp output reports after the relay has started (see SPEC.md)
enum HidStep : uint8_t {
  HID_STEP_NONE = 0,           // Not preparing
  HID_STEP_DISCOVER,           // Discovering the services (EspBle)
  HID_STEP_READ_REPORT_MAP,    // Reading the Report Map (EspBle)
  HID_STEP_READ_REFERENCE,     // Reading the Report Reference of each Report (EspBle)
  HID_STEP_CHECK_DESCRIPTORS,  // Checking the descriptors at the remembered handles (NimBLE)
  HID_STEP_CHECK_REFERENCE,    // Checking the Report Reference at the remembered handle (NimBLE)
  HID_STEP_SUBSCRIBE,          // Subscribing to each input report (NimBLE)
};
struct HidReport {
  uint16_t handle;                 // Value handle of the Report characteristic
  uint16_t referenceHandle;        // Handle of its Report Reference descriptor (0 if it has none)
  uint16_t cccdHandle;             // Handle of its Client Characteristic Configuration descriptor (0 if it has none)
  bool notifiable;
  bool writable;                   // With Write Without Response
  uint8_t id;                      // Report ID and type, from the Report Reference
  uint8_t type;
  // Layouts of a keyboard input report and of a media key report to subscribe to, and of a lamp output report to
  // write to (NULL for the others)
  const UsageFormat *keyboard;
  const UsageFormat *consumer;
  const UsageFormat *lamp;
  bool ready;                      // Prepared: its notifications are relayed, or the lamps are written to it
  // Media keys pressed in its last notification
  PressedConsumerKeys consumerPressed;
};
static HidStep hidStep;
static EspBleConnectionId hidConnectionId;  // The connection whose HID service is prepared (0 if none)
static bool hidForPairing;                  // Prepared to remember it while pairing, not to relay
static bool hidCheckReports;                // The Reports are the remembered ones, checked before subscribing
static bool hidLatePhase;                   // Preparing the media key and lamp reports, after the relay has started
static size_t hidStepIndex;                 // The Report being read, checked or subscribed to
static uint8_t hidCheckedDescriptors;       // Descriptors found as remembered for the Report being checked
static uint16_t hidReportMapHandle;
static uint8_t hidReportMap[REPORT_MAP_MAX_SIZE];  // The Report Map read, kept to be remembered
static size_t hidReportMapSize;                    // 0 if it is too long to be kept
static HidReport hidReports[HID_MAX_REPORTS];
static size_t hidReportCount;
static UsageFormat keyboardFormats[KEYBOARD_MAX_FORMATS];
static size_t keyboardFormatCount;
static UsageFormat consumerFormats[CONSUMER_MAX_FORMATS];
static size_t consumerFormatCount;
static UsageFormat lampFormats[LAMP_MAX_FORMATS];
static size_t lampFormatCount;

// Lamps last written to the BLE keyboard on this connection (-1 if none), as hostLamps
static int16_t writtenLamps = -1;

// The HID service of a keyboard, remembered in the NVS so that the connections need no discovery (see SPEC.md): the
// handles of the keyboard input reports, the media key reports and the lamp output reports, and the Report Map that
// tells their layout
const size_t HID_CACHE_MAX_REPORTS = KEYBOARD_MAX_FORMATS + CONSUMER_MAX_FORMATS + LAMP_MAX_FORMATS;
struct CachedReport {
  uint16_t valueHandle;
  uint16_t cccdHandle;  // 0 for an output report
  uint16_t referenceHandle;
  uint8_t id;
  uint8_t type;
};
struct HidCache {
  uint8_t version;      // HID_CACHE_VERSION (0 if there is none)
  uint8_t addressType;  // Of the keyboard, as in SPEC.md
  uint8_t address[6];   // Little endian
  uint8_t reportCount;
  CachedReport reports[HID_CACHE_MAX_REPORTS];
  uint16_t reportMapSize;
  uint8_t reportMap[REPORT_MAP_MAX_SIZE];
};
static const size_t HID_CACHE_HEADER_SIZE = offsetof(HidCache, reportMap);
static HidCache hidCache;      // Of the registered keyboard
static HidCache pairingCache;  // Found while pairing, saved when the registration has been replaced

// Notifications and results of the GATT operations made with NimBLE directly, which arrive in the NimBLE host task
enum NimbleEventType : uint8_t {
  NIMBLE_NOTIFICATION,  // A notification or an indication
  NIMBLE_DESCRIPTOR,    // A descriptor found by a discovery
  NIMBLE_DONE,          // An operation has ended (status 0 on success); the value for a read
};
struct NimbleEvent {
  NimbleEventType type;
  uint32_t operation;  // Tag of the operation (0 for a notification)
  uint16_t connectionHandle;
  uint16_t handle;     // Of the attribute
  int status;          // 0 on success, otherwise a NimBLE error
  uint16_t uuid16;     // NIMBLE_DESCRIPTOR: the UUID if it is a 16-bit one, otherwise 0
  uint8_t length;
  uint8_t data[NIMBLE_EVENT_MAX_DATA];
};
static QueueHandle_t nimbleEvents;
static struct ble_gap_event_listener nimbleListener;
static uint32_t nimbleOperation;  // Tag of the operation whose result is awaited

// Output report from the host: the lamps (runs in the USB task, so only keep them here; loop() writes them to the BLE
// keyboard)
void keyboardSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
    hostLamps = buffer[0];
  }
}

// Output report from the settings page (runs in the USB task, so only queue the request here)
void settingsSetReport(uint8_t reportId, hid_report_type_t type, uint8_t const *buffer, uint16_t size) {
  if (type != HID_REPORT_TYPE_OUTPUT || reportId != SETTINGS_REPORT_ID) {
    return;
  }
  uint8_t request[SETTINGS_REPORT_SIZE] = {};
  memcpy(request, buffer, size < sizeof(request) ? size : sizeof(request));
  xQueueSend(settingsRequests, request, 0);
}

// Make the NVS hold only the data of this firmware: erase it on the first run (when the ID is missing)
static bool initNvs() {
  Preferences prefs;
  uint32_t id = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    id = prefs.getUInt(NVS_KEY_ID, 0);
    prefs.end();
  }
  if (id == NVS_ID) {
    return true;
  }
  if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) {
    return false;
  }
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  bool written = prefs.putUInt(NVS_KEY_ID, NVS_ID) == sizeof(uint32_t);
  prefs.end();
  return written;
}

// Load the name of the registered keyboard from the NVS (none if it is missing or broken)
static void loadKeyboardName() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return;
  }
  KeyboardName loaded = {};
  size_t size = prefs.getBytes(NVS_KEY_KEYBOARD, &loaded, sizeof(loaded));
  prefs.end();
  if (size >= KEYBOARD_NAME_HEADER_SIZE && loaded.nameSize <= SCAN_NAME_MAX_SIZE &&
      size == KEYBOARD_NAME_HEADER_SIZE + loaded.nameSize) {
    keyboardName = loaded;
  }
}

// Save the name of the keyboard just registered with the address of its bond
static bool saveKeyboardName(const EspBleBond &bond, const char *name, size_t nameSize) {
  KeyboardName saved = {};
  if (!parseAddress(bond.peerAddress, saved.address)) {
    return false;
  }
  saved.addressType = static_cast<uint8_t>(bond.peerAddressType);
  saved.nameSize = nameSize;
  memcpy(saved.name, name, nameSize);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  size_t size = KEYBOARD_NAME_HEADER_SIZE + nameSize;
  bool written = prefs.putBytes(NVS_KEY_KEYBOARD, &saved, size) == size;
  prefs.end();
  if (written) {
    keyboardName = saved;
  }
  return written;
}

// Load the remembered HID service from the NVS (none if it is missing, broken or of another layout)
static void loadHidCache() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return;
  }
  static HidCache loaded;
  memset(&loaded, 0, sizeof(loaded));
  size_t size = prefs.getBytes(NVS_KEY_HID_CACHE, &loaded, sizeof(loaded));
  prefs.end();
  if (size >= HID_CACHE_HEADER_SIZE && loaded.version == HID_CACHE_VERSION && loaded.reportCount >= 1 &&
      loaded.reportCount <= HID_CACHE_MAX_REPORTS && loaded.reportMapSize <= REPORT_MAP_MAX_SIZE &&
      size == HID_CACHE_HEADER_SIZE + loaded.reportMapSize) {
    hidCache = loaded;
  }
}

// Save the remembered HID service with the address of the keyboard of the bond
static bool saveHidCache(HidCache &cache, const EspBleBond &bond) {
  if (!parseAddress(bond.peerAddress, cache.address)) {
    return false;
  }
  cache.addressType = static_cast<uint8_t>(bond.peerAddressType);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  size_t size = HID_CACHE_HEADER_SIZE + cache.reportMapSize;
  bool written = prefs.putBytes(NVS_KEY_HID_CACHE, &cache, size) == size;
  prefs.end();
  if (written) {
    hidCache = cache;
  }
  return written;
}

// Forget the remembered HID service, so that the next connection discovers it and remembers it again
static void forgetHidCache() {
  hidCache.version = 0;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.remove(NVS_KEY_HID_CACHE);
    prefs.end();
  }
}

// Whether the remembered HID service is that of the keyboard of the bond
static bool hidCacheMatches(const EspBleBond &bond) {
  uint8_t address[6];
  return hidCache.version == HID_CACHE_VERSION && parseAddress(bond.peerAddress, address) &&
         hidCache.addressType == static_cast<uint8_t>(bond.peerAddressType) &&
         memcmp(hidCache.address, address, 6) == 0;
}

// Wait until the HID interface can send a report (gives up after about 100 ms)
static bool waitReady(Adafruit_USBD_HID &hid) {
  for (int i = 0; i < 100; i++) {
    if (hid.ready()) {
      return true;
    }
    delay(1);
  }
  return false;
}

// Parse an address written as "aa:bb:cc:dd:ee:ff" (most significant byte first) into little endian
static bool parseAddress(const String &text, uint8_t *address) {
  return sscanf(text.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &address[5], &address[4], &address[3],
                &address[2], &address[1], &address[0]) == 6;
}

// Format a little-endian address as "aa:bb:cc:dd:ee:ff" (most significant byte first), as EspBle does
static String formatAddress(uint8_t const *address) {
  char text[18];
  snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[5], address[4], address[3], address[2],
           address[1], address[0]);
  return String(text);
}

// Length of the UTF-8 text cut to at most maxSize bytes without splitting a character
static size_t truncateUtf8(const char *text, size_t size, size_t maxSize) {
  if (size <= maxSize) {
    return size;
  }
  size_t cut = maxSize;
  while (cut > 0 && (text[cut] & 0xC0) == 0x80) {
    cut--;
  }
  return cut;
}

// Whether the advertisement is from a connectable device that looks like a keyboard (see SPEC.md)
static bool isKeyboardAdvertisement(const EspBleScanResult &result) {
  if (!result.connectable) {
    return false;
  }
  if (result.appearance == APPEARANCE_KEYBOARD) {
    return true;
  }
  return result.advertisesService(UUID_HID_SERVICE) &&
         (result.appearance == 0 || result.appearance == APPEARANCE_GENERIC_HID);
}

// The device with the address in the scan result (NULL if it is not there)
static ScannedDevice *findScannedDevice(uint8_t addressType, uint8_t const *address) {
  for (uint8_t i = 0; i < scannedCount; i++) {
    if (scannedDevices[i].addressType == addressType && memcmp(scannedDevices[i].address, address, 6) == 0) {
      return &scannedDevices[i];
    }
  }
  return NULL;
}

// Add the device to the scan result, or update it if it is already there (see SPEC.md)
static void onScanResult(const EspBleScanResult &result) {
  uint8_t address[6];
  if (!parseAddress(result.address, address)) {
    return;
  }
  uint8_t addressType = static_cast<uint8_t>(result.addressType);
  ScannedDevice *device = findScannedDevice(addressType, address);
  if (device == NULL) {
    if (scannedCount >= SCAN_MAX_DEVICES || !isKeyboardAdvertisement(result)) {
      return;
    }
    device = &scannedDevices[scannedCount++];
    memset(device, 0, sizeof(*device));
    device->addressType = addressType;
    memcpy(device->address, address, 6);
  }
  device->rssi = static_cast<int8_t>(result.rssi);
  if (result.hasName()) {
    const char *name = result.name.c_str();
    size_t nameSize = truncateUtf8(name, result.name.length(), SCAN_NAME_MAX_SIZE);
    // A shortened name is the beginning of the complete name (Bluetooth Core Specification), and EspBle does not
    // tell them apart: keep the complete name, e.g. from the scan response, when only the advertisement comes
    bool shortened = nameSize < device->nameSize && memcmp(device->name, name, nameSize) == 0;
    if (!shortened) {
      device->nameSize = nameSize;
      memcpy(device->name, name, nameSize);
    }
  }
}

// Start the search with the settings of SPEC.md
static bool beginScan() {
  EspBleScanConfig config;
  // Also receive the scan responses, which often carry the name
  config.active = true;
  // Receive every advertisement: the controller drops repeats by address alone, so without this a device
  // first seen before it advertises as a keyboard (e.g. before entering pairing mode) would never be listed
  config.wantDuplicates = true;
  return ble.scanner().start(config);
}

// Clear the scan result and start the search (restarts it if it is running). A search cannot start while connecting
// (BLE_HS_EBUSY), so during an attempt to connect to the registered keyboard it starts when the attempt ends
static bool startScan() {
  scannedCount = 0;
  if (keyboardConnecting) {
    scanRequested = true;
    return true;
  }
  return beginScan();
}

// Stop the search, including a requested one
static bool stopScan() {
  scanRequested = false;
  return ble.scanner().stop();
}

// Whether the search is running or requested
static bool isScanning() {
  return scanRequested || ble.scanner().isScanning();
}

// Whether the pairing is in progress (see SPEC.md)
static bool isPairing() {
  return pairingState == PAIRING_CONNECTING || pairingState == PAIRING_PAIRING || pairingState == PAIRING_PASSKEY;
}

// Whether the two bonds are of the same device
static bool isSameBond(const EspBleBond &a, const EspBleBond &b) {
  return a.peerAddressType == b.peerAddressType && a.peerAddress.equalsIgnoreCase(b.peerAddress);
}

// Read all the bonds (false if they cannot be read)
static bool readBonds(EspBleBond *bonds, size_t &count) {
  count = min(ble.bondCount(), BOND_CAPACITY);
  for (size_t i = 0; i < count; i++) {
    if (!ble.bond(i, bonds[i])) {
      return false;
    }
  }
  return true;
}

// Whether the bond was there when the pairing started
static bool isBondBeforePairing(const EspBleBond &bond) {
  for (size_t i = 0; i < bondsBeforePairingCount; i++) {
    if (isSameBond(bondsBeforePairing[i], bond)) {
      return true;
    }
  }
  return false;
}

// Delete the bond of a device that is not connected, even while connected to another one. EspBle's deleteBond() refuses
// while any connection is open, but NimBLE only deletes what is stored for the device, without touching the
// connections (ble_gap_unpair() does the same after closing only the connection to that device)
static bool deleteOldBond(const EspBleBond &bond) {
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

// Leave only the bond made by the pairing, so that it replaces the registration. The others are deleted without closing
// the connection of the pairing
static bool replaceRegistration() {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return false;
  }
  bool made = false;
  for (size_t i = 0; i < count; i++) {
    made = made || !isBondBeforePairing(bonds[i]);
  }
  if (!made) {
    // The pairing has updated a bond with an address seen before: it can be told apart only if it is the only one
    return count == 1;
  }
  for (size_t i = 0; i < count; i++) {
    if (isBondBeforePairing(bonds[i]) && !deleteOldBond(bonds[i])) {
      return false;
    }
  }
  return true;
}

// Replace the registration with the bond made by the pairing, and save with it the name in the scan result and the HID
// service found while pairing. The name is only for showing, and the HID service is discovered on the first connection
// if it is not remembered, so a failure to save them does not undo the registration
static bool registerPairedKeyboard() {
  bool replaced = replaceRegistration();
  EspBleBond registered[BOND_CAPACITY];
  size_t registeredCount;
  if (replaced && readBonds(registered, registeredCount) && registeredCount == 1) {
    saveKeyboardName(registered[0], pairingTargetName, pairingTargetNameSize);
    if (pairingCache.version == HID_CACHE_VERSION) {
      saveHidCache(pairingCache, registered[0]);
    }
  }
  return replaced;
}

// Delete the bond made by the pairing, so that the registration stays as it was (needs no connection to delete a bond)
static bool deletePairingBond() {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (!isBondBeforePairing(bonds[i]) && !ble.deleteBond(bonds[i])) {
      return false;
    }
  }
  return true;
}

// Start pairing with the device in the scan result (see SPEC.md). The connection is made in loop() by
// preparePairing(), after the relay has stopped
static uint8_t startPairing(uint8_t const *args) {
  if (isPairing()) {
    return RESULT_BLE_FAILED;
  }
  const ScannedDevice *device = findScannedDevice(args[0], args + 1);
  if (device == NULL) {
    return RESULT_INVALID_ARGUMENT;
  }
  pairingTarget.peerAddress = formatAddress(device->address);
  pairingTarget.peerAddressType = static_cast<EspBleAddressType>(device->addressType);
  pairingTargetNameSize = device->nameSize;
  memcpy(pairingTargetName, device->name, device->nameSize);
  pairingPasskey = 0;
  pairingConnectionId = 0;
  pairingCancelRequested = false;
  pairingBonded = false;
  pairingNotKeyboard = false;
  pairingCache.version = 0;
  pairingPreparing = true;
  pairingState = PAIRING_CONNECTING;
  scanRequested = false;
  // A bond can be deleted only without a connection
  if (keyboardConnectionId != 0) {
    ble.disconnect(keyboardConnectionId);
  }
  return RESULT_SUCCESS;
}

// Connect to the device to pair with once there is neither a connection nor an attempt to connect
static void preparePairing() {
  if (!pairingPreparing) {
    return;
  }
  if (pairingCancelRequested) {
    pairingPreparing = false;
    pairingState = PAIRING_IDLE;
    return;
  }
  if (keyboardConnecting || keyboardConnectionId != 0 || ble.connectionCount() != 0) {
    return;
  }
  pairingPreparing = false;

  // Delete the bond of the device itself, or the stack would only encrypt with the old keys instead of pairing.
  // Keep the others until the pairing succeeds (see SPEC.md)
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count)) {
    pairingState = PAIRING_REGISTRATION_FAILED;
    return;
  }
  bondsBeforePairingCount = 0;
  for (size_t i = 0; i < count; i++) {
    if (!isSameBond(bonds[i], pairingTarget)) {
      bondsBeforePairing[bondsBeforePairingCount++] = bonds[i];
    } else if (!ble.deleteBond(bonds[i])) {
      pairingState = PAIRING_REGISTRATION_FAILED;
      return;
    }
  }

  // connect() stops the search first, and the pairing starts as soon as connected (pairOnConnect)
  if (!ble.connect(pairingTarget.peerAddress.c_str(), pairingTarget.peerAddressType)) {
    pairingState = PAIRING_CONNECT_FAILED;
  }
}

// Cancel the pairing, which ends when the connection is closed (see SPEC.md)
static void cancelPairing() {
  if (!isPairing()) {
    return;
  }
  pairingCancelRequested = true;
  // A connection attempt cannot be canceled: onPairingConnected() closes the connection if it is made, and
  // preparePairing() ends the preparation
  if (pairingConnectionId != 0) {
    ble.disconnect(pairingConnectionId);
  }
}

// Connected to the device to pair with
static void onPairingConnected(const EspBleConnection &connection) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingConnectionId = connection.id;
  if (pairingCancelRequested) {
    ble.disconnect(connection.id);
    return;
  }
  pairingState = PAIRING_PAIRING;
}

// Failed to connect to the device to pair with
static void onPairingConnectionFailed(const EspBleConnectionFailure &failure) {
  if (pairingState != PAIRING_CONNECTING) {
    return;
  }
  pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_CONNECT_FAILED;
}

// The passkey to be typed on the keyboard (Passkey Entry)
static void onPairingPasskey(const EspBlePasskeyDisplayed &event) {
  if (event.connection.id != pairingConnectionId || pairingCancelRequested) {
    return;
  }
  pairingPasskey = event.passkey;
  pairingState = PAIRING_PASSKEY;
}

// The pairing has ended. With a bond, the HID service is found to remember it and to see that the device is a
// keyboard, and then the registration is replaced and the connection goes on to the relay (see SPEC.md)
static void onPairingSecurityChanged(const EspBleSecurityChanged &event) {
  if (event.connection.id != pairingConnectionId || hidConnectionId == pairingConnectionId) {
    return;
  }
  pairingBonded = event.success && event.connection.bonded;
  pairingState = PAIRING_PAIRING;
  if (pairingBonded && !pairingCancelRequested) {
    pairingConnection = event.connection;
    hidConnectionId = pairingConnectionId;
    hidForPairing = true;
    pairingShortInterval = askShortInterval(pairingConnectionId, event.connection.supervisionTimeout);
    startHidDiscovery();
    return;
  }
  ble.disconnect(pairingConnectionId);
}

// The connection to the device to pair with is closed before the relay has taken it over: the end of the pairing
static void onPairingDisconnected(const EspBleConnection &connection) {
  if (pairingConnectionId == 0 || connection.id != pairingConnectionId) {
    return;
  }
  pairingConnectionId = 0;
  if (hidConnectionId == connection.id) {
    hidConnectionId = 0;
    hidStep = HID_STEP_NONE;
  }
  if (pairingBonded && pairingNotKeyboard) {
    pairingState = deletePairingBond() ? PAIRING_NOT_KEYBOARD : PAIRING_REGISTRATION_FAILED;
  } else if (pairingBonded) {
    pairingState = registerPairedKeyboard() ? PAIRING_REGISTERED : PAIRING_REGISTRATION_FAILED;
  } else {
    pairingState = pairingCancelRequested ? PAIRING_IDLE : PAIRING_PAIR_FAILED;
  }
}

// The registered keyboard: the only bond (none if there is no bond or more than one, see SPEC.md)
static bool readRegisteredKeyboard(EspBleBond &keyboard) {
  EspBleBond bonds[BOND_CAPACITY];
  size_t count;
  if (!readBonds(bonds, count) || count != 1) {
    return false;
  }
  keyboard = bonds[0];
  return true;
}

// Keep trying to connect to the registered keyboard, but not while searching or pairing (see SPEC.md)
static void connectKeyboard() {
  if (keyboardConnecting || keyboardConnectionId != 0 || isPairing() || isScanning()) {
    return;
  }
  EspBleBond registered;
  if (!readRegisteredKeyboard(registered)) {
    return;
  }
  keyboardTarget = registered;
  keyboardConnecting =
      ble.connect(registered.peerAddress.c_str(), registered.peerAddressType, KEYBOARD_CONNECT_ATTEMPT_MS);
}

// Value of a field of a HID report, least significant bit first
static uint32_t readReportBits(uint8_t const *data, uint32_t bitOffset, uint8_t bitSize) {
  uint32_t value = 0;
  for (uint8_t bit = 0; bit < bitSize; bit++) {
    uint32_t source = bitOffset + bit;
    if (data[source / 8] & (1 << (source % 8))) {
      value |= 1UL << bit;
    }
  }
  return value;
}

// Value of a two's complement number of the size in bits
static int32_t signExtend(uint32_t value, uint8_t bits) {
  if (bits == 0 || bits >= 32) {
    return static_cast<int32_t>(value);
  }
  uint32_t sign = 1UL << (bits - 1);
  return static_cast<int32_t>((value ^ sign) - sign);
}

// Write a field of a HID report, least significant bit first
static void writeReportBits(uint8_t *data, uint32_t bitOffset, uint8_t bitSize, uint32_t value) {
  for (uint8_t bit = 0; bit < bitSize; bit++) {
    uint32_t target = bitOffset + bit;
    if (value & (1UL << bit)) {
      data[target / 8] |= 1 << (target % 8);
    } else {
      data[target / 8] &= ~(1 << (target % 8));
    }
  }
}

// Read the layout of the reports of the main item (input or output) from a Report Map (HID 1.11, 6.2.2): the fields of
// the usages on the page in the application collections of the usage (the keys of a keyboard: Keyboard/Keypad page,
// the media keys of a consumer control: Consumer page, the lamps of a keyboard: LED page). Returns the number of layouts
static size_t parseUsageFormats(uint8_t const *map, size_t size, uint32_t collection, uint16_t usagePage, uint8_t item,
                                UsageFormat *formats, size_t maxFormats) {
  // Global items, which Push and Pop save and restore
  struct Globals {
    uint16_t usagePage;
    uint32_t logicalMinimum;
    uint8_t logicalMinimumSize;
    uint32_t logicalMaximum;
    uint8_t logicalMaximumSize;
    uint32_t reportSize;
    uint32_t reportCount;
    uint8_t reportId;
  };
  const size_t MAX_PUSHED = 4;
  Globals globals = {};
  Globals pushed[MAX_PUSHED];
  size_t pushedCount = 0;

  // Local items. A usage of 4 bytes holds its page in the upper half; a shorter one is on the Usage Page in effect at
  // the main item (HID 1.11, 6.2.2.8)
  struct Usage {
    uint32_t value;
    bool extended;
  };
  Usage usages[FIELD_MAX_USAGES];
  size_t usageCount = 0;
  bool usagesCut = false;
  Usage usageMinimum = {};
  Usage usageMaximum = {};
  bool usageRange = false;
  auto fullUsage = [&globals](const Usage &usage) {
    return usage.extended ? usage.value : (static_cast<uint32_t>(globals.usagePage) << 16) | usage.value;
  };

  // Whether each open collection is in an application collection of the usage
  const size_t MAX_DEPTH = 8;
  bool targetCollection[MAX_DEPTH] = {};
  size_t depth = 0;
  auto inTarget = [&]() { return depth > 0 && targetCollection[min(depth, MAX_DEPTH) - 1]; };

  // Length of each input report so far, in bits
  const size_t MAX_REPORT_IDS = 16;
  const uint32_t MAX_REPORT_BITS = 0xFFFF;
  uint8_t reportIds[MAX_REPORT_IDS];
  uint32_t reportBits[MAX_REPORT_IDS];
  size_t reportIdCount = 0;

  size_t formatCount = 0;
  for (size_t offset = 0; offset < size;) {
    uint8_t prefix = map[offset++];
    // Long item (none is defined): skip it
    if (prefix == 0xFE) {
      if (offset + 2 > size) {
        break;
      }
      offset += 2 + map[offset];
      continue;
    }
    uint8_t dataSize = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
    if (offset + dataSize > size) {
      break;
    }
    uint32_t data = 0;
    for (uint8_t i = 0; i < dataSize; i++) {
      data |= static_cast<uint32_t>(map[offset + i]) << (8 * i);
    }
    offset += dataSize;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = prefix >> 4;

    if (type == 1) {  // Global
      switch (tag) {
        case 0: globals.usagePage = data; break;
        case 1: globals.logicalMinimum = data; globals.logicalMinimumSize = dataSize; break;
        case 2: globals.logicalMaximum = data; globals.logicalMaximumSize = dataSize; break;
        case 7: globals.reportSize = data; break;
        case 8: globals.reportId = data; break;
        case 9: globals.reportCount = data; break;
        case 10: if (pushedCount < MAX_PUSHED) pushed[pushedCount++] = globals; break;  // Push
        case 11: if (pushedCount > 0) globals = pushed[--pushedCount]; break;          // Pop
      }
      continue;
    }
    if (type == 2) {  // Local
      Usage usage = {data, dataSize == 4};
      if (tag == 0) {
        if (usageCount < FIELD_MAX_USAGES) {
          usages[usageCount++] = usage;
        } else {
          usagesCut = true;
        }
      } else if (tag == 1) {
        usageMinimum = usage;
        usageRange = true;
      } else if (tag == 2) {
        usageMaximum = usage;
      }
      continue;
    }
    if (type != 0) {
      continue;
    }

    // Main
    if (tag == 10) {  // Collection
      // Application collection (1) of the usage
      bool target = inTarget() || (data == 1 && usageCount > 0 && fullUsage(usages[0]) == collection);
      if (depth < MAX_DEPTH) {
        targetCollection[depth] = target;
      }
      depth++;
    } else if (tag == 12) {  // End Collection
      if (depth > 0) {
        depth--;
      }
    } else if (tag == item) {  // Input or Output, as asked
      // Where this item starts in its report
      size_t report = 0;
      while (report < reportIdCount && reportIds[report] != globals.reportId) {
        report++;
      }
      if (report == reportIdCount && reportIdCount < MAX_REPORT_IDS) {
        reportIds[reportIdCount] = globals.reportId;
        reportBits[reportIdCount++] = 0;
      }
      if (report < reportIdCount) {
        uint32_t bitOffset = reportBits[report];
        uint32_t itemBits = globals.reportSize <= 32 && globals.reportCount <= MAX_REPORT_BITS
                                ? globals.reportSize * globals.reportCount
                                : MAX_REPORT_BITS + 1;
        reportBits[report] = min(bitOffset + itemBits, MAX_REPORT_BITS + 1);

        // Keys or lamps: data (bit 0 clear) in a collection of the usage, on the page asked
        uint32_t firstUsage = usageRange ? fullUsage(usageMinimum) : usageCount > 0 ? fullUsage(usages[0]) : 0;
        UsageFormat *format = NULL;
        if (inTarget() && (data & 0x01) == 0 && (firstUsage >> 16) == usagePage && globals.reportSize >= 1 &&
            globals.reportCount >= 1 && bitOffset + itemBits <= MAX_REPORT_BITS) {
          for (size_t i = 0; i < formatCount; i++) {
            if (formats[i].reportId == globals.reportId) {
              format = &formats[i];
            }
          }
          if (format == NULL && formatCount < maxFormats) {
            format = &formats[formatCount++];
            memset(format, 0, sizeof(*format));
            format->reportId = globals.reportId;
          }
        }
        if (format != NULL && format->fieldCount < FORMAT_MAX_FIELDS) {
          UsageField &field = format->fields[format->fieldCount++];
          field.bitOffset = bitOffset;
          field.bitSize = globals.reportSize;
          field.count = globals.reportCount;
          field.array = (data & 0x02) == 0;
          field.logicalMinimum = signExtend(globals.logicalMinimum, 8 * globals.logicalMinimumSize);
          // Unsigned unless the minimum is negative, so that a maximum such as 255 written in one byte is not -1
          field.logicalMaximum = field.logicalMinimum < 0
                                     ? signExtend(globals.logicalMaximum, 8 * globals.logicalMaximumSize)
                                     : static_cast<int32_t>(globals.logicalMaximum);
          field.usageMinimum = usageRange ? fullUsage(usageMinimum) & 0xFFFF : 0;
          field.usageMaximum = usageRange ? fullUsage(usageMaximum) & 0xFFFF : 0;
          field.usageCount = usageRange ? 0 : usageCount;
          field.usagesCut = !usageRange && usagesCut;
          for (size_t i = 0; i < field.usageCount; i++) {
            field.usages[i] = fullUsage(usages[i]) & 0xFFFF;
          }
        }
      }
    }
    // Local items apply only to the main item that follows them
    usageCount = 0;
    usagesCut = false;
    usageMinimum = {};
    usageMaximum = {};
    usageRange = false;
  }

  for (size_t i = 0; i < formatCount; i++) {
    for (size_t report = 0; report < reportIdCount; report++) {
      if (reportIds[report] == formats[i].reportId) {
        formats[i].bitLength = min(reportBits[report], MAX_REPORT_BITS);
      }
    }
  }
  return formatCount;
}

// The usage of an element of a field: the key or lamp of the element at the index of a variable field, or the key or
// lamp at the index held by an element of an array (0 if there is none)
static uint16_t fieldUsage(const UsageField &field, uint32_t index) {
  if (field.usageCount == 0) {
    return field.usageMinimum <= field.usageMaximum && index <= static_cast<uint32_t>(field.usageMaximum -
                                                                                      field.usageMinimum)
               ? field.usageMinimum + index
               : 0;
  }
  if (index < field.usageCount) {
    return field.usages[index];
  }
  // The last usage also applies to the remaining elements (HID 1.11, 6.2.2.8), unless more were listed than kept
  return field.array || field.usagesCut ? 0 : field.usages[field.usageCount - 1];
}

// Whether the length of a report is that of its layout in the Report Map
static bool hasReportLength(const UsageFormat &format, size_t length) {
  return length == static_cast<size_t>((format.bitLength + 7) / 8);
}

// Call the handler with the usage of each key pressed in a report, in the order of the report
static void readPressedKeys(const UsageFormat &format, uint8_t const *data, PressedKeyHandler handler, void *context) {
  for (uint8_t f = 0; f < format.fieldCount; f++) {
    const UsageField &field = format.fields[f];
    for (uint32_t i = 0; i < field.count; i++) {
      uint32_t value = readReportBits(data, field.bitOffset + i * field.bitSize, field.bitSize);
      uint16_t usage;
      if (field.array) {
        int32_t index = field.logicalMinimum < 0 ? signExtend(value, field.bitSize) : static_cast<int32_t>(value);
        // An out-of-range value is no key (HID 1.11, 6.2.2.5)
        if (index < field.logicalMinimum || index > field.logicalMaximum) {
          continue;
        }
        usage = fieldUsage(field, index - field.logicalMinimum);
      } else {
        if (value == 0) {
          continue;
        }
        usage = fieldUsage(field, i);
      }
      if (usage != 0) {
        handler(usage, context);
      }
    }
  }
}

// Read the keys of a keyboard input report into a boot keyboard report (see SPEC.md). False if the length is not that
// of the Report Map
static bool readKeyboardReport(const UsageFormat &format, uint8_t const *data, size_t length, KeyboardReport &report) {
  if (!hasReportLength(format, length)) {
    return false;
  }
  // The pressed usages, one bit each
  uint8_t pressed[32] = {};
  readPressedKeys(format, data,
                  [](uint16_t usage, void *context) {
                    if (usage < 256) {
                      static_cast<uint8_t *>(context)[usage / 8] |= 1 << (usage % 8);
                    }
                  },
                  pressed);
  auto isPressed = [&pressed](unsigned usage) { return (pressed[usage / 8] & (1 << (usage % 8))) != 0; };

  report = {};
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (isPressed(USAGE_FIRST_MODIFIER + bit)) {
      report.modifiers |= 1 << bit;
    }
  }
  size_t count = 0;
  for (unsigned usage = USAGE_FIRST_KEY; usage <= USAGE_LAST_KEY; usage++) {
    if (isPressed(usage)) {
      if (count < KEYBOARD_REPORT_KEYS) {
        report.keys[count] = usage;
      }
      count++;
    }
  }
  // Too many keys at once: ErrorRollOver in every slot, with the modifiers still reported (HID 1.11, Appendix C). An
  // error that the keyboard reports is passed on in the same way
  uint8_t error = count > KEYBOARD_REPORT_KEYS ? USAGE_ERROR_ROLL_OVER : 0;
  for (uint8_t usage = USAGE_ERROR_ROLL_OVER; usage <= USAGE_ERROR_UNDEFINED; usage++) {
    if (isPressed(usage)) {
      error = usage;
      break;
    }
  }
  if (error != 0) {
    memset(report.keys, error, sizeof(report.keys));
  }
  return true;
}

// Whether the usage is among the usages
static bool containsUsage(uint16_t const *usages, uint8_t count, uint16_t usage) {
  for (uint8_t i = 0; i < count; i++) {
    if (usages[i] == usage) {
      return true;
    }
  }
  return false;
}

// Read the media keys pressed in a media key report: those that the host can be sent (see SPEC.md). False if the
// length is not that of the Report Map
static bool readConsumerReport(const UsageFormat &format, uint8_t const *data, size_t length,
                               PressedConsumerKeys &pressed) {
  if (!hasReportLength(format, length)) {
    return false;
  }
  pressed.count = 0;
  readPressedKeys(format, data,
                  [](uint16_t usage, void *context) {
                    PressedConsumerKeys &keys = *static_cast<PressedConsumerKeys *>(context);
                    if (usage <= USAGE_LAST_CONSUMER && keys.count < CONSUMER_MAX_PRESSED &&
                        !containsUsage(keys.usages, keys.count, usage)) {
                      keys.usages[keys.count++] = usage;
                    }
                  },
                  &pressed);
  return true;
}

// The media key to send after a media key report, as the guidelines of Microsoft (see SPEC.md): the key newly pressed
// (the first in the report if more than one), or none if the key sent has been released; otherwise the key sent
static uint16_t chooseConsumerUsage(uint16_t sent, const PressedConsumerKeys &before, const PressedConsumerKeys &now) {
  for (uint8_t i = 0; i < now.count; i++) {
    if (!containsUsage(before.usages, before.count, now.usages[i])) {
      return now.usages[i];
    }
  }
  if (containsUsage(before.usages, before.count, sent) && !containsUsage(now.usages, now.count, sent)) {
    return 0;
  }
  return sent;
}

// The index of the usage among the usages of a field (false if the field has no such usage)
static bool fieldUsageIndex(const UsageField &field, uint16_t usage, uint32_t &index) {
  if (field.usageCount == 0) {
    if (usage < field.usageMinimum || usage > field.usageMaximum) {
      return false;
    }
    index = usage - field.usageMinimum;
    return true;
  }
  for (uint8_t i = 0; i < field.usageCount; i++) {
    if (field.usages[i] == usage) {
      index = i;
      return true;
    }
  }
  return false;
}

// Make a lamp output report from the lamps of the host (see SPEC.md): each lamp that the host sends, Num Lock to Kana,
// at its place, and 0 for the rest. Returns the length (0 if the report is longer than the buffer)
static size_t makeLampReport(const UsageFormat &format, uint8_t lamps, uint8_t *data, size_t maxSize) {
  size_t length = (format.bitLength + 7) / 8;
  if (length == 0 || length > maxSize) {
    return 0;
  }
  memset(data, 0, length);
  auto isLit = [lamps](uint16_t usage) {
    return usage >= USAGE_FIRST_LAMP && usage <= USAGE_LAST_LAMP && (lamps & (1 << (usage - USAGE_FIRST_LAMP))) != 0;
  };
  for (uint8_t f = 0; f < format.fieldCount; f++) {
    const UsageField &field = format.fields[f];
    if (!field.array) {
      // Each element is a lamp: 1 if lit
      for (uint32_t i = 0; i < field.count; i++) {
        if (isLit(fieldUsage(field, i))) {
          writeReportBits(data, field.bitOffset + i * field.bitSize, field.bitSize, 1);
        }
      }
      continue;
    }
    // Each element holds the index of a lit lamp, in the order of the usages, while elements remain
    uint32_t element = 0;
    for (uint16_t usage = USAGE_FIRST_LAMP; usage <= USAGE_LAST_LAMP && element < field.count; usage++) {
      uint32_t index;
      if (!isLit(usage) || !fieldUsageIndex(field, usage, index)) {
        continue;
      }
      int64_t value = static_cast<int64_t>(field.logicalMinimum) + index;
      if (value > field.logicalMaximum) {
        continue;
      }
      writeReportBits(data, field.bitOffset + element * field.bitSize, field.bitSize, static_cast<uint32_t>(value));
      element++;
    }
  }
  return length;
}

// Send the queued reports one at a time whenever the host can take one: the keyboard reports on the keyboard, and the
// media key reports on the settings HID. While the host is not ready (e.g. suspended), the reports are discarded
static void sendReports() {
  if (!TinyUSBDevice.ready()) {
    keyboardQueue.count = 0;
    consumerQueue.count = 0;
    return;
  }
  if (keyboardQueue.count > 0 && keyboard.ready()) {
    KeyboardReport report = keyboardQueue.front();
    if (keyboard.keyboardReport(0, report.modifiers, report.keys)) {
      keyboardQueue.pop();
    }
  }
  if (consumerQueue.count > 0 && settings.ready()) {
    if (settings.sendReport16(CONSUMER_REPORT_ID, consumerQueue.front())) {
      consumerQueue.pop();
    }
  }
}

// Whether a UUID that EspBle reports (the 128-bit form on the Bluetooth base UUID) is the 16-bit UUID written in 4
// hex digits
static bool isUuid16(const String &uuid, const char *uuid16) {
  return uuid.equalsIgnoreCase(uuid16) ||
         uuid.equalsIgnoreCase(String("0000") + uuid16 + "-0000-1000-8000-00805f9b34fb");
}

// Read the layouts of the keyboard input reports, of the media key reports and of the lamp output reports from a Report
// Map
static void parseHidFormats(uint8_t const *map, size_t size) {
  keyboardFormatCount = parseUsageFormats(map, size, COLLECTION_KEYBOARD, USAGE_PAGE_KEYBOARD, ITEM_INPUT,
                                          keyboardFormats, KEYBOARD_MAX_FORMATS);
  consumerFormatCount = parseUsageFormats(map, size, COLLECTION_CONSUMER_CONTROL, USAGE_PAGE_CONSUMER, ITEM_INPUT,
                                          consumerFormats, CONSUMER_MAX_FORMATS);
  lampFormatCount =
      parseUsageFormats(map, size, COLLECTION_KEYBOARD, USAGE_PAGE_LED, ITEM_OUTPUT, lampFormats, LAMP_MAX_FORMATS);
}

// The layout of the report with the ID among the layouts (NULL if there is none)
static const UsageFormat *findFormat(const UsageFormat *formats, size_t count, uint8_t reportId) {
  for (size_t i = 0; i < count; i++) {
    if (formats[i].reportId == reportId) {
      return &formats[i];
    }
  }
  return NULL;
}

// Take the layouts of the Report from its type and ID: a keyboard input report or a media key report if it is an input
// report that can be subscribed to, and a lamp output report if it is an output report that can be written to
static void findReportFormats(HidReport &report) {
  bool input = report.type == REPORT_TYPE_INPUT && report.notifiable && report.cccdHandle != 0;
  bool output = report.type == REPORT_TYPE_OUTPUT && report.writable;
  report.keyboard = input ? findFormat(keyboardFormats, keyboardFormatCount, report.id) : NULL;
  report.consumer = input ? findFormat(consumerFormats, consumerFormatCount, report.id) : NULL;
  report.lamp = output ? findFormat(lampFormats, lampFormatCount, report.id) : NULL;
}

// Whether the Report is used: a keyboard input report, a media key report or a lamp output report
static bool isUsedReport(const HidReport &report) {
  return report.keyboard != NULL || report.consumer != NULL || report.lamp != NULL;
}

// The HID service cannot be prepared: close the connection. For the relay, the next attempt tries again. For the
// pairing, the device is registered without remembering its HID service, unless the HID service shows that the device
// is not a keyboard (unusable), see SPEC.md
static void failHidPreparation(bool unusable) {
  hidStep = HID_STEP_NONE;
  if (hidForPairing) {
    pairingNotKeyboard = unusable;
  }
  ble.disconnect(hidConnectionId);
}

// Ask for a short connection interval for the preparation of the HID service (see PREPARATION_INTERVAL_MIN)
static bool askShortInterval(EspBleConnectionId id, uint16_t supervisionTimeout) {
  return ble.updateConnectionParameters(id, PREPARATION_INTERVAL_MIN, PREPARATION_INTERVAL_MAX, 0, supervisionTimeout);
}

// Discover the HID service with EspBle (the first step without a remembered HID service)
static void startHidDiscovery() {
  hidStep = HID_STEP_DISCOVER;
  hidCheckReports = false;
  hidReportMapSize = 0;
  if (!ble.discoverServices(hidConnectionId)) {
    failHidPreparation(false);
  }
}

// Find the Report Map and the Reports of the first HID service among the discovered services
static bool findHidReports() {
  EspBleConnectionId id = hidConnectionId;
  // The service starts at its handle and ends before the next service
  uint16_t first = 0;
  uint16_t next = 0;
  EspBleGattServiceInfo service;
  for (size_t i = 0; ble.discoveredService(id, i, service); i++) {
    if (isUuid16(service.serviceUuid, UUID_HID_SERVICE) && (first == 0 || service.handle < first)) {
      first = service.handle;
    }
  }
  if (first == 0) {
    return false;
  }
  for (size_t i = 0; ble.discoveredService(id, i, service); i++) {
    if (service.handle > first && (next == 0 || service.handle < next)) {
      next = service.handle;
    }
  }
  auto inService = [first, next](uint16_t handle) { return handle > first && (next == 0 || handle < next); };

  hidReportMapHandle = 0;
  hidReportCount = 0;
  EspBleGattCharacteristicInfo characteristic;
  for (size_t i = 0; ble.discoveredCharacteristic(id, i, characteristic, UUID_HID_SERVICE); i++) {
    if (!inService(characteristic.handle)) {
      continue;
    }
    if (isUuid16(characteristic.characteristicUuid, UUID_REPORT_MAP) && hidReportMapHandle == 0) {
      hidReportMapHandle = characteristic.handle;
    } else if (isUuid16(characteristic.characteristicUuid, UUID_REPORT) && hidReportCount < HID_MAX_REPORTS) {
      HidReport &report = hidReports[hidReportCount++];
      report = {};
      report.handle = characteristic.handle;
      report.notifiable = characteristic.notifiable;
      report.writable = characteristic.writableWithoutResponse;
    }
  }
  // All Reports have the same UUID, so their descriptors are matched by the handle of the Report
  EspBleGattDescriptorInfo descriptor;
  for (size_t i = 0; ble.discoveredDescriptor(id, i, descriptor, UUID_HID_SERVICE, UUID_REPORT); i++) {
    bool reference = isUuid16(descriptor.descriptorUuid, UUID_REPORT_REFERENCE);
    bool cccd = isUuid16(descriptor.descriptorUuid, UUID_CCCD);
    for (size_t r = 0; r < hidReportCount; r++) {
      if (hidReports[r].handle != descriptor.characteristicHandle) {
        continue;
      }
      if (reference) {
        hidReports[r].referenceHandle = descriptor.handle;
      } else if (cccd) {
        hidReports[r].cccdHandle = descriptor.handle;
      }
    }
  }
  return hidReportMapHandle != 0 && hidReportCount > 0;
}

// What to remember of the HID service just discovered: the keyboard input reports, the media key reports, the lamp
// output reports and the Report Map. The address is set when it is saved (false if it cannot be kept)
static bool buildHidCache(HidCache &cache) {
  memset(&cache, 0, sizeof(cache));
  if (hidReportMapSize == 0) {
    return false;
  }
  for (size_t i = 0; i < hidReportCount; i++) {
    const HidReport &report = hidReports[i];
    if (!isUsedReport(report)) {
      continue;
    }
    if (cache.reportCount >= HID_CACHE_MAX_REPORTS) {
      return false;
    }
    cache.reports[cache.reportCount++] = {report.handle, report.cccdHandle, report.referenceHandle, report.id,
                                          report.type};
  }
  if (cache.reportCount == 0) {
    return false;
  }
  cache.reportMapSize = hidReportMapSize;
  memcpy(cache.reportMap, hidReportMap, hidReportMapSize);
  cache.version = HID_CACHE_VERSION;
  return true;
}

// Take the remembered HID service as the one to prepare (false if its Report Map has no layout for a report)
static bool useHidCache() {
  parseHidFormats(hidCache.reportMap, hidCache.reportMapSize);
  hidReportCount = 0;
  for (size_t i = 0; i < hidCache.reportCount; i++) {
    const CachedReport &cached = hidCache.reports[i];
    HidReport &report = hidReports[hidReportCount++];
    report = {};
    report.handle = cached.valueHandle;
    report.referenceHandle = cached.referenceHandle;
    report.cccdHandle = cached.cccdHandle;
    report.notifiable = cached.type == REPORT_TYPE_INPUT;
    report.writable = cached.type == REPORT_TYPE_OUTPUT;
    report.id = cached.id;
    report.type = cached.type;
    findReportFormats(report);
    if (!isUsedReport(report)) {
      return false;
    }
  }
  return hidReportCount > 0;
}

// The remembered HID service is not that of the keyboard: discover it as on the first connection, and remember it again
// when the relay starts. Once relaying, forget it and close the connection instead, so that the next connection
// discovers it without stopping the relay in the middle (see SPEC.md)
static void rediscoverHidService() {
  if (hidLatePhase) {
    forgetHidCache();
    failHidPreparation(false);
    return;
  }
  keyboardUsesCache = false;
  if (!keyboardShortInterval) {
    keyboardShortInterval = askShortInterval(keyboardConnectionId, keyboardSupervisionTimeout);
  }
  startHidDiscovery();
}

// Tag passed to a NimBLE GATT operation, and back to loop() with its result
static void *nimbleOperationTag(uint32_t operation) {
  return reinterpret_cast<void *>(static_cast<uintptr_t>(operation));
}

static uint32_t nimbleOperationOf(void *tag) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tag));
}

// The NimBLE host task passes the events to loop() (an event is lost if the queue is full)
static void pushNimbleEvent(const NimbleEvent &event) {
  xQueueSend(nimbleEvents, &event, 0);
}

// Copy the value of an mbuf into the event (status BLE_HS_EMSGSIZE if it is too long)
static void copyNimbleData(const struct os_mbuf *om, NimbleEvent &event) {
  uint16_t length = om == NULL ? 0 : OS_MBUF_PKTLEN(om);
  uint16_t copied = 0;
  if (length > NIMBLE_EVENT_MAX_DATA || (length > 0 && ble_hs_mbuf_to_flat(om, event.data, length, &copied) != 0)) {
    event.status = BLE_HS_EMSGSIZE;
    return;
  }
  event.length = copied;
}

// NimBLE host task: a notification or an indication from a peer (EspBle passes on only those it subscribed to)
static int onNimbleGapEvent(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_NOTIFY_RX) {
    return 0;
  }
  NimbleEvent result = {};
  result.type = NIMBLE_NOTIFICATION;
  result.connectionHandle = event->notify_rx.conn_handle;
  result.handle = event->notify_rx.attr_handle;
  copyNimbleData(event->notify_rx.om, result);
  pushNimbleEvent(result);
  return 0;
}

// NimBLE host task: a descriptor found by ble_gattc_disc_all_dscs(), or the end of the discovery
static int onNimbleDescriptor(uint16_t connectionHandle, const struct ble_gatt_error *error, uint16_t valueHandle,
                              const struct ble_gatt_dsc *descriptor, void *tag) {
  NimbleEvent result = {};
  result.operation = nimbleOperationOf(tag);
  result.connectionHandle = connectionHandle;
  if (error->status == 0 && descriptor != NULL) {
    result.type = NIMBLE_DESCRIPTOR;
    result.handle = descriptor->handle;
    result.uuid16 = descriptor->uuid.u.type == BLE_UUID_TYPE_16 ? descriptor->uuid.u16.value : 0;
  } else {
    result.type = NIMBLE_DONE;
    result.status = error->status == BLE_HS_EDONE ? 0 : error->status;
  }
  pushNimbleEvent(result);
  return 0;
}

// NimBLE host task: the result of ble_gattc_read() or ble_gattc_write_flat()
static int onNimbleAttribute(uint16_t connectionHandle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attribute, void *tag) {
  NimbleEvent result = {};
  result.type = NIMBLE_DONE;
  result.operation = nimbleOperationOf(tag);
  result.connectionHandle = connectionHandle;
  result.status = error->status;
  if (error->status == 0 && attribute != NULL) {
    result.handle = attribute->handle;
    copyNimbleData(attribute->om, result);
  }
  pushNimbleEvent(result);
  return 0;
}

// The relay starts. The parameters of the connection are put back if they were changed for the preparation, and the
// HID service is remembered if it was discovered
static void startRelaying() {
  keyboardReady = true;
  // So that the relay runs as before the preparation was shortened
  if (keyboardShortInterval) {
    ble.updateConnectionParameters(keyboardConnectionId, keyboardInterval, keyboardInterval, keyboardLatency,
                                   keyboardSupervisionTimeout);
  }
  if (!keyboardUsesCache) {
    static HidCache cache;
    if (buildHidCache(cache)) {
      saveHidCache(cache, keyboardTarget);
    }
  }
}

// Whether the Report is prepared in the current phase: the keyboard input reports, then the media key reports and the
// lamp output reports (a report with both keys and media keys is prepared with the keyboard input reports)
static bool isPhaseReport(const HidReport &report) {
  return hidLatePhase ? report.keyboard == NULL && (report.consumer != NULL || report.lamp != NULL)
                      : report.keyboard != NULL;
}

// Skip to the next Report of the current phase
static void skipToPhaseReport() {
  while (hidStepIndex < hidReportCount && !isPhaseReport(hidReports[hidStepIndex])) {
    hidStepIndex++;
  }
}

// Prepare the Reports of the current phase: check them first if they are the remembered ones, then subscribe
static void prepareReports() {
  hidStepIndex = 0;
  if (hidCheckReports) {
    checkNextCachedReport();
  } else {
    subscribeReports();
  }
}

// Subscribe to the Reports of the current phase
static void subscribeReports() {
  hidStep = HID_STEP_SUBSCRIBE;
  hidStepIndex = 0;
  subscribeNextReport();
}

// Subscribe to the next input report of the current phase by writing its Client Characteristic Configuration (an output
// report is ready without it). When all the keyboard input reports are subscribed, the relay starts and the media key
// reports and the lamp output reports are prepared (see SPEC.md)
static void subscribeNextReport() {
  skipToPhaseReport();
  while (hidStepIndex < hidReportCount && hidReports[hidStepIndex].type != REPORT_TYPE_INPUT) {
    hidReports[hidStepIndex].ready = true;
    hidStepIndex++;
    skipToPhaseReport();
  }
  if (hidStepIndex >= hidReportCount) {
    if (hidLatePhase) {
      hidStep = HID_STEP_NONE;
      return;
    }
    startRelaying();
    hidLatePhase = true;
    prepareReports();
    return;
  }
  static const uint8_t NOTIFICATIONS_ENABLED[] = {0x01, 0x00};
  if (ble_gattc_write_flat(keyboardConnectionHandle, hidReports[hidStepIndex].cccdHandle, NOTIFICATIONS_ENABLED,
                           sizeof(NOTIFICATIONS_ENABLED), onNimbleAttribute,
                           nimbleOperationTag(++nimbleOperation)) != 0) {
    failHidPreparation(false);
  }
}

// Check the next remembered Report of the current phase: the descriptors at its handles, then its Report Reference.
// Subscribe when all are as remembered
static void checkNextCachedReport() {
  skipToPhaseReport();
  if (hidStepIndex >= hidReportCount) {
    subscribeReports();
    return;
  }
  const HidReport &report = hidReports[hidStepIndex];
  hidStep = HID_STEP_CHECK_DESCRIPTORS;
  hidCheckedDescriptors = 0;
  if (ble_gattc_disc_all_dscs(keyboardConnectionHandle, report.handle, max(report.cccdHandle, report.referenceHandle),
                              onNimbleDescriptor, nimbleOperationTag(++nimbleOperation)) != 0) {
    rediscoverHidService();
  }
}

// Relay on the connection of the pairing, which is encrypted with the bond now registered and whose HID service is
// found. Closing it would make some keyboards wait for a key before they accept a connection again (see SPEC.md)
static void relayPairingConnection(const EspBleBond &registered) {
  keyboardConnectionId = pairingConnectionId;
  keyboardConnectionHandle = pairingConnection.handle;
  keyboardTarget = registered;
  keyboardReady = false;
  // Not remembered again when the relay starts, unless it could not be saved with the registration
  keyboardUsesCache = hidCacheMatches(registered);
  keyboardShortInterval = pairingShortInterval;
  keyboardInterval = pairingConnection.connectionInterval;
  keyboardLatency = pairingConnection.peripheralLatency;
  keyboardSupervisionTimeout = pairingConnection.supervisionTimeout;
  pairingConnectionId = 0;
  hidForPairing = false;
  writtenLamps = -1;
  // The Report References have just been read while pairing
  hidCheckReports = false;
  hidLatePhase = false;
  subscribeReports();
}

// The HID service of the device paired with is found: remember it, replace the registration, and relay on this
// connection. If canceled, or if the registration cannot be replaced, close the connection instead, and the
// registration is replaced after the disconnection
static void finishPairingPreparation() {
  hidStep = HID_STEP_NONE;
  buildHidCache(pairingCache);
  EspBleBond registered;
  if (pairingCancelRequested || !registerPairedKeyboard() || !readRegisteredKeyboard(registered)) {
    ble.disconnect(hidConnectionId);
    return;
  }
  pairingState = PAIRING_REGISTERED;
  relayPairingConnection(registered);
}

// Read the Report Reference of the next Report that has one and can be notified or written without response (the others
// cannot be keyboard input reports, media key reports or lamp output reports). When all are read, subscribe to the
// Reports, or end the pairing
static void readNextReportReference() {
  while (hidStepIndex < hidReportCount &&
         (hidReports[hidStepIndex].referenceHandle == 0 ||
          (!hidReports[hidStepIndex].notifiable && !hidReports[hidStepIndex].writable))) {
    hidStepIndex++;
  }
  if (hidStepIndex < hidReportCount) {
    if (!ble.readDescriptor(hidConnectionId, hidReports[hidStepIndex].referenceHandle)) {
      failHidPreparation(false);
    }
    return;
  }
  bool found = false;
  for (size_t i = 0; i < hidReportCount; i++) {
    found = found || hidReports[i].keyboard != NULL;
  }
  if (!found) {
    failHidPreparation(true);
    return;
  }
  if (hidForPairing) {
    finishPairingPreparation();
    return;
  }
  subscribeReports();
}

// A result of a NimBLE GATT operation of the preparation
static void onNimbleResult(const NimbleEvent &event) {
  if (hidForPairing || hidConnectionId != keyboardConnectionId || event.connectionHandle != keyboardConnectionHandle ||
      hidStepIndex >= hidReportCount) {
    return;
  }
  HidReport &report = hidReports[hidStepIndex];
  switch (hidStep) {
    case HID_STEP_CHECK_DESCRIPTORS:
      if (event.type == NIMBLE_DESCRIPTOR) {
        if ((event.handle == report.cccdHandle && event.uuid16 == UUID16_CCCD) ||
            (event.handle == report.referenceHandle && event.uuid16 == UUID16_REPORT_REFERENCE)) {
          hidCheckedDescriptors++;
        }
        return;
      }
      // The Report Reference, and the Client Characteristic Configuration of an input report
      if (event.status != 0 || hidCheckedDescriptors != (report.cccdHandle != 0 ? 2 : 1)) {
        rediscoverHidService();
        return;
      }
      hidStep = HID_STEP_CHECK_REFERENCE;
      if (ble_gattc_read(keyboardConnectionHandle, report.referenceHandle, onNimbleAttribute,
                         nimbleOperationTag(++nimbleOperation)) != 0) {
        rediscoverHidService();
      }
      return;
    case HID_STEP_CHECK_REFERENCE:
      if (event.status != 0 || event.length < 2 || event.data[0] != report.id || event.data[1] != report.type) {
        rediscoverHidService();
        return;
      }
      hidStepIndex++;
      checkNextCachedReport();
      return;
    case HID_STEP_SUBSCRIBE:
      if (event.status != 0) {
        failHidPreparation(false);
        return;
      }
      report.ready = true;
      hidStepIndex++;
      subscribeNextReport();
      return;
    default:
      return;
  }
}

// Relay a media key report: send the media key chosen as the guidelines of Microsoft (see SPEC.md)
static void relayConsumerReport(HidReport &hidReport, uint8_t const *data, size_t length) {
  PressedConsumerKeys pressed;
  if (!readConsumerReport(*hidReport.consumer, data, length, pressed)) {
    return;
  }
  consumerQueue.push(chooseConsumerUsage(consumerQueue.last, hidReport.consumerPressed, pressed));
  hidReport.consumerPressed = pressed;
}

// Relay a keyboard input report of the registered keyboard as a boot keyboard report, and a media key report as the
// media key (see SPEC.md). Only the connection encrypted with the bond gets ready to relay, and only the Reports
// subscribed to after their handles have been checked are relayed (a bonded keyboard may notify before that)
static void relayNotification(const NimbleEvent &event) {
  if (!keyboardReady || event.connectionHandle != keyboardConnectionHandle || event.status != 0) {
    return;
  }
  for (size_t i = 0; i < hidReportCount; i++) {
    HidReport &hidReport = hidReports[i];
    if (hidReport.handle != event.handle || !hidReport.ready) {
      continue;
    }
    KeyboardReport report;
    if (hidReport.keyboard != NULL && readKeyboardReport(*hidReport.keyboard, event.data, event.length, report)) {
      keyboardQueue.push(report);
    }
    if (hidReport.consumer != NULL) {
      relayConsumerReport(hidReport, event.data, event.length);
    }
    return;
  }
}

// Write the lamps of the host to the lamp output reports of the BLE keyboard when they change, and once the reports are
// ready on a connection (see SPEC.md). Written without response, so that no GATT operation of the preparation waits;
// if a write cannot be queued, all are written again in the next loop()
static void writeLamps() {
  uint8_t lamps = hostLamps;
  if (!keyboardReady || lamps == writtenLamps) {
    return;
  }
  bool written = false;
  for (size_t i = 0; i < hidReportCount; i++) {
    const HidReport &report = hidReports[i];
    if (report.lamp == NULL || !report.ready) {
      continue;
    }
    uint8_t data[LAMP_REPORT_MAX_SIZE];
    size_t length = makeLampReport(*report.lamp, lamps, data, sizeof(data));
    if (length == 0) {
      continue;
    }
    if (ble_gattc_write_no_rsp_flat(keyboardConnectionHandle, report.handle, data, length) != 0) {
      return;
    }
    written = true;
  }
  if (written) {
    writtenLamps = lamps;
  }
}

// Handle the notifications and the results of the NimBLE GATT operations in loop(). A result of an operation that is
// no longer awaited (e.g. one of a closed connection) is ignored
static void handleNimbleEvents() {
  NimbleEvent event;
  while (xQueueReceive(nimbleEvents, &event, 0) == pdTRUE) {
    if (event.type == NIMBLE_NOTIFICATION) {
      relayNotification(event);
    } else if (event.operation == nimbleOperation) {
      onNimbleResult(event);
    }
  }
}

// Connected to the registered keyboard; the encryption with the bond starts by itself (pairOnConnect). Without a
// remembered HID service, ask for a short connection interval for its discovery. A remembered one needs only a few
// round trips, which end before a new interval would take effect. If the pairing has started during the attempt, close
// the connection so that the pairing can go on
static void onKeyboardConnected(const EspBleConnection &connection) {
  keyboardConnectionId = connection.id;
  keyboardConnectionHandle = connection.handle;
  keyboardReady = false;
  writtenLamps = -1;
  keyboardShortInterval = false;
  if (isPairing()) {
    ble.disconnect(connection.id);
    return;
  }
  keyboardInterval = connection.connectionInterval;
  keyboardLatency = connection.peripheralLatency;
  keyboardSupervisionTimeout = connection.supervisionTimeout;
  keyboardUsesCache = hidCacheMatches(keyboardTarget);
  if (!keyboardUsesCache) {
    keyboardShortInterval = askShortInterval(connection.id, connection.supervisionTimeout);
  }
}

// Encrypted with the bond: prepare the HID service to receive the input, with the remembered one if any. Otherwise the
// device is not the registered keyboard (or has lost the keys), so close the connection
static void onKeyboardSecurityChanged(const EspBleSecurityChanged &event) {
  if (!event.success || !event.connection.bonded) {
    ble.disconnect(event.connection.id);
    return;
  }
  // Once for a connection
  if (hidConnectionId == keyboardConnectionId || keyboardReady) {
    return;
  }
  hidConnectionId = keyboardConnectionId;
  hidForPairing = false;
  hidLatePhase = false;
  if (!keyboardUsesCache) {
    startHidDiscovery();
  } else if (!useHidCache()) {
    rediscoverHidService();
  } else {
    hidCheckReports = true;
    prepareReports();
  }
}

// The services are discovered: read the Report Map of the HID service. Without the HID service, the device is not a
// keyboard
static void onServicesDiscovered(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_DISCOVER || result.connectionId != hidConnectionId) {
    return;
  }
  if (!result.success) {
    failHidPreparation(false);
    return;
  }
  if (!findHidReports()) {
    failHidPreparation(true);
    return;
  }
  hidStep = HID_STEP_READ_REPORT_MAP;
  if (!ble.readCharacteristic(hidConnectionId, hidReportMapHandle)) {
    failHidPreparation(false);
  }
}

// The Report Map is read: keep it to be remembered, find the layout of the keyboard input reports and of the media key
// reports, then read the Report References. Without a keyboard input in the Report Map, the device is not a keyboard
static void onCharacteristicRead(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_READ_REPORT_MAP || result.connectionId != hidConnectionId ||
      result.handle != hidReportMapHandle) {
    return;
  }
  if (!result.success) {
    failHidPreparation(false);
    return;
  }
  uint8_t const *map = reinterpret_cast<uint8_t const *>(result.value.c_str());
  hidReportMapSize = result.value.length() <= REPORT_MAP_MAX_SIZE ? result.value.length() : 0;
  memcpy(hidReportMap, map, hidReportMapSize);
  parseHidFormats(map, result.value.length());
  if (keyboardFormatCount == 0) {
    failHidPreparation(true);
    return;
  }
  hidStep = HID_STEP_READ_REFERENCE;
  hidStepIndex = 0;
  readNextReportReference();
}

// A Report Reference is read: the Report is a keyboard input report or a media key report if it is an input report with
// the ID of one in the Report Map and can be subscribed to, and a lamp output report if it is an output report with the
// ID of one and can be written to
static void onDescriptorRead(const EspBleGattResult &result) {
  if (hidStep != HID_STEP_READ_REFERENCE || result.connectionId != hidConnectionId ||
      hidStepIndex >= hidReportCount || result.descriptorHandle != hidReports[hidStepIndex].referenceHandle) {
    return;
  }
  if (!result.success || result.value.length() < 2) {
    failHidPreparation(false);
    return;
  }
  HidReport &report = hidReports[hidStepIndex];
  report.id = static_cast<uint8_t>(result.value[0]);
  report.type = static_cast<uint8_t>(result.value[1]);
  findReportFormats(report);
  hidStepIndex++;
  readNextReportReference();
}

// The connection to the registered keyboard is closed: release every key and media key on the host
static void onKeyboardDisconnected(const EspBleConnection &connection) {
  keyboardConnectionId = 0;
  keyboardReady = false;
  if (hidConnectionId == connection.id) {
    hidConnectionId = 0;
    hidStep = HID_STEP_NONE;
  }
  hidLatePhase = false;
  hidReportCount = 0;
  keyboardQueue.push(KeyboardReport{});
  consumerQueue.push(0);
}

// Events of the connections, for the registered keyboard or for the device to pair with. Only one attempt to connect
// runs at a time, and the keyboard is not connected to while pairing
static void onConnected(const EspBleConnection &connection) {
  if (keyboardConnecting) {
    keyboardConnecting = false;
    onKeyboardConnected(connection);
  } else {
    onPairingConnected(connection);
  }
}

static void onConnectionFailed(const EspBleConnectionFailure &failure) {
  if (keyboardConnecting) {
    keyboardConnecting = false;
  } else {
    onPairingConnectionFailed(failure);
  }
}

static void onSecurityChanged(const EspBleSecurityChanged &event) {
  if (keyboardConnectionId != 0 && event.connection.id == keyboardConnectionId) {
    onKeyboardSecurityChanged(event);
  } else {
    onPairingSecurityChanged(event);
  }
}

static void onDisconnected(const EspBleConnection &connection) {
  if (keyboardConnectionId != 0 && connection.id == keyboardConnectionId) {
    onKeyboardDisconnected(connection);
  } else {
    onPairingDisconnected(connection);
  }
}

// Send a response to the settings page
static void sendSettingsResponse(uint8_t command, uint8_t result, uint8_t const *data, size_t size) {
  uint8_t response[SETTINGS_REPORT_SIZE] = {command, result};
  if (size > 0) {
    memcpy(response + 2, data, size);
  }
  if (!waitReady(settings)) {
    return;
  }
  settings.sendReport(SETTINGS_REPORT_ID, response, sizeof(response));
}

// Send the device at the index in the scan result (see SPEC.md)
static void sendScanResult(uint8_t command, uint8_t index) {
  uint8_t data[SCAN_RESULT_HEADER_SIZE + SCAN_NAME_MAX_SIZE] = {};
  data[0] = isScanning() ? 1 : 0;
  data[1] = scannedCount;
  size_t size = 2;
  if (index < scannedCount) {
    const ScannedDevice &device = scannedDevices[index];
    data[2] = device.addressType;
    memcpy(data + 3, device.address, 6);
    data[9] = static_cast<uint8_t>(device.rssi);
    data[10] = device.nameSize;
    memcpy(data + SCAN_RESULT_HEADER_SIZE, device.name, device.nameSize);
    size = SCAN_RESULT_HEADER_SIZE + device.nameSize;
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, size);
}

// Send the pairing state (see SPEC.md)
static void sendPairingState(uint8_t command) {
  uint8_t data[PAIRING_STATE_SIZE] = {pairingState};
  uint32_t passkey = pairingState == PAIRING_PASSKEY ? pairingPasskey : 0;
  for (size_t i = 0; i < 4; i++) {
    data[1 + i] = (passkey >> (8 * i)) & 0xFF;
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, sizeof(data));
}

// The state of the connection to the registered keyboard (see SPEC.md)
static KeyboardState keyboardState() {
  if (keyboardConnectionId != 0) {
    return keyboardReady ? KEYBOARD_RELAYING : KEYBOARD_PREPARING;
  }
  return keyboardConnecting ? KEYBOARD_CONNECTING : KEYBOARD_IDLE;
}

// Send the registered keyboard and the state of the connection to it (see SPEC.md)
static void sendRegistration(uint8_t command) {
  uint8_t data[REGISTRATION_HEADER_SIZE + SCAN_NAME_MAX_SIZE] = {};
  size_t size = 1;
  EspBleBond registered;
  if (readRegisteredKeyboard(registered) && parseAddress(registered.peerAddress, data + 2)) {
    data[0] = 1;
    data[1] = static_cast<uint8_t>(registered.peerAddressType);
    data[8] = keyboardState();
    size = REGISTRATION_HEADER_SIZE;
    // The saved name, only if it is that of the registered keyboard
    if (keyboardName.addressType == data[1] && memcmp(keyboardName.address, data + 2, 6) == 0) {
      data[9] = keyboardName.nameSize;
      memcpy(data + REGISTRATION_HEADER_SIZE, keyboardName.name, keyboardName.nameSize);
      size += keyboardName.nameSize;
    }
  }
  sendSettingsResponse(command, RESULT_SUCCESS, data, size);
}

// Handle a request from the settings page
static void handleSettingsRequest(uint8_t const *request) {
  uint8_t command = request[0];
  switch (command) {
    case COMMAND_RESTART:
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      // Wait until the host has received the response
      waitReady(settings);
      ESP.restart();
      break;
    case COMMAND_START_SCAN:
      // Not while pairing (see SPEC.md)
      sendSettingsResponse(command, !isPairing() && startScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_STOP_SCAN:
      sendSettingsResponse(command, stopScan() ? RESULT_SUCCESS : RESULT_BLE_FAILED, NULL, 0);
      break;
    case COMMAND_GET_SCAN_RESULT:
      sendScanResult(command, request[1]);
      break;
    case COMMAND_START_PAIRING:
      sendSettingsResponse(command, startPairing(request + 1), NULL, 0);
      break;
    case COMMAND_CANCEL_PAIRING:
      cancelPairing();
      sendSettingsResponse(command, RESULT_SUCCESS, NULL, 0);
      break;
    case COMMAND_GET_PAIRING_STATE:
      sendPairingState(command);
      break;
    case COMMAND_GET_REGISTRATION:
      sendRegistration(command);
      break;
    default:
      sendSettingsResponse(command, RESULT_UNKNOWN_COMMAND, NULL, 0);
      break;
  }
}

void setup() {
  // Before anything else uses the NVS (the BLE bonds are kept there)
  initNvs();
  loadKeyboardName();
  loadHidCache();

  settingsRequests = xQueueCreate(4, SETTINGS_REPORT_SIZE);
  nimbleEvents = xQueueCreate(NIMBLE_EVENT_QUEUE_SIZE, sizeof(NimbleEvent));

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(productName, sizeof(productName), "BLE HID to USB HID Bridge (%02X%02X)", mac[4], mac[5]);

  // PID pre-allocated by Espressif for TinyUSB: 0x4000 | CDC (bit 0) | HID (bit 2).
  // Change it when the USB classes change.
  TinyUSBDevice.setID(0x303A, 0x4005);
  TinyUSBDevice.setProductDescriptor(productName);

  keyboard.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  keyboard.setReportDescriptor(descKeyboard, sizeof(descKeyboard));
  keyboard.setStringDescriptor(productName);
  keyboard.setReportCallback(NULL, keyboardSetReport);
  keyboard.begin();

  settings.setReportDescriptor(descSettings, sizeof(descSettings));
  settings.setStringDescriptor(productName);
  settings.setReportCallback(NULL, settingsSetReport);
  settings.begin();

  // USB has already started before setup() (CDC on boot), so re-enumerate to apply the settings above
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  // Pair as the side that only displays the passkey, requesting MITM protection: a keyboard that can type gets
  // Passkey Entry (the passkey is shown on the settings page), and the others get Just Works
  EspBleConfig config;
  config.security.enabled = true;
  config.security.bonding = true;
  config.security.mitm = true;
  config.security.ioCapability = EspBleSecurityIoCapability::DisplayOnly;
  // The scan results and the events of the connections are delivered from ble.update() in loop()
  ble.begin(config);
  // The keyboard input reports are subscribed to with NimBLE directly, so their notifications are received here too
  ble_gap_event_listener_register(&nimbleListener, onNimbleGapEvent, NULL);
  ble.scanner().onResult(onScanResult);
  ble.onConnected(onConnected);
  ble.onConnectionFailed(onConnectionFailed);
  ble.onPasskeyDisplayed(onPairingPasskey);
  ble.onSecurityChanged(onSecurityChanged);
  ble.onDisconnected(onDisconnected);
  ble.onServicesDiscovered(onServicesDiscovered);
  ble.onCharacteristicRead(onCharacteristicRead);
  ble.onDescriptorRead(onDescriptorRead);
}

void loop() {
  uint8_t request[SETTINGS_REPORT_SIZE];
  if (xQueueReceive(settingsRequests, request, 0) == pdTRUE) {
    handleSettingsRequest(request);
  }

  ble.update();
  handleNimbleEvents();

  // A search requested during an attempt to connect starts when the attempt has ended
  if (scanRequested && !keyboardConnecting) {
    scanRequested = false;
    beginScan();
  }
  preparePairing();
  connectKeyboard();
  sendReports();
  writeLamps();

  delay(LOOP_INTERVAL_MS);
}
