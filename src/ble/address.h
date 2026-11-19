// Addresses of BLE devices: the text form of EspBle ("aa:bb:cc:dd:ee:ff", most significant byte first) and the bytes in
// little endian of the settings HID protocol (see SPEC.md)

#pragma once

#include <Arduino.h>

// Parse an address written as "aa:bb:cc:dd:ee:ff" (most significant byte first) into little endian
bool parseAddress(const String &text, uint8_t *address);

// Format a little-endian address as "aa:bb:cc:dd:ee:ff" (most significant byte first), as EspBle does
String formatAddress(uint8_t const *address);
