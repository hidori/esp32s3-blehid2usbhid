// What is kept in the NVS besides the bonds: the name of the registered keyboard, and its HID service remembered (see
// SPEC.md)

#pragma once

#include <EspBle.h>

#include "scan.h"
#include "service.h"

// Name of the registered keyboard, kept in the NVS with its address, since a bond has no name. The name is valid only
// while the address is that of the registered keyboard (see SPEC.md)
struct KeyboardName {
  uint8_t addressType;  // As in SPEC.md
  uint8_t address[6];   // Little endian
  uint8_t nameSize;
  char name[SCAN_NAME_MAX_SIZE];  // UTF-8, not terminated
};

// Make the NVS hold only the data of this firmware: erase it on the first run (when the ID is missing)
bool initNvs();

// The name of the registered keyboard: load it, get it, save it with the address of the bond, and forget it
void loadKeyboardName();
const KeyboardName &getKeyboardName();
bool saveKeyboardName(const EspBleBond &bond, const char *name, size_t nameSize);
void forgetKeyboardName();

// The remembered HID service: load it, get it (version 0 if there is none), save it with the address of the keyboard of
// the bond, forget it, and see whether it is that of the keyboard of the bond
void loadHidCache();
const HidCache &getHidCache();
bool saveHidCache(HidCache &cache, const EspBleBond &bond);
void forgetHidCache();
bool hidCacheMatches(const EspBleBond &bond);
