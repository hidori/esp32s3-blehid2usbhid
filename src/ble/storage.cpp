// What is kept in the NVS besides the bonds (see storage.h)

#include "storage.h"

#include <Preferences.h>
#include <nvs_flash.h>

#include "address.h"

// NVS (the ID marks that the NVS has been initialized by this firmware)
static const char NVS_NAMESPACE[] = "settings";
static const char NVS_KEY_ID[] = "id";
static const char NVS_KEY_KEYBOARD[] = "keyboard";   // Address and name of the registered keyboard
static const char NVS_KEY_HID_CACHE[] = "hidcache";  // The HID service of the registered keyboard, remembered
static const uint32_t NVS_ID = 0xB1E2B5B0;

// Name of the registered keyboard
static KeyboardName keyboardName;
static const size_t KEYBOARD_NAME_HEADER_SIZE = offsetof(KeyboardName, name);

// HID service of the registered keyboard, remembered
static const size_t HID_CACHE_HEADER_SIZE = offsetof(HidCache, reportMap);
static HidCache hidCache;

// Make the NVS hold only the data of this firmware: erase it on the first run (when the ID is missing)
bool initNvs() {
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
void loadKeyboardName() {
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
bool saveKeyboardName(const EspBleBond &bond, const char *name, size_t nameSize) {
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

// Forget the name of the registered keyboard
void forgetKeyboardName() {
  keyboardName = {};
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.remove(NVS_KEY_KEYBOARD);
    prefs.end();
  }
}

// Load the remembered HID service from the NVS (none if it is missing, broken or of another layout)
void loadHidCache() {
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
bool saveHidCache(HidCache &cache, const EspBleBond &bond) {
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
void forgetHidCache() {
  hidCache.version = 0;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.remove(NVS_KEY_HID_CACHE);
    prefs.end();
  }
}

// Whether the remembered HID service is that of the keyboard of the bond
bool hidCacheMatches(const EspBleBond &bond) {
  uint8_t address[6];
  return hidCache.version == HID_CACHE_VERSION && parseAddress(bond.peerAddress, address) &&
         hidCache.addressType == static_cast<uint8_t>(bond.peerAddressType) &&
         memcmp(hidCache.address, address, 6) == 0;
}

const KeyboardName &getKeyboardName() {
  return keyboardName;
}

const HidCache &getHidCache() {
  return hidCache;
}
