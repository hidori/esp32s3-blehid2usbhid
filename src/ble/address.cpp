// Addresses of BLE devices (see address.h)

#include "address.h"

// Parse an address written as "aa:bb:cc:dd:ee:ff" (most significant byte first) into little endian
bool parseAddress(const String &text, uint8_t *address) {
  return sscanf(text.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &address[5], &address[4], &address[3],
                &address[2], &address[1], &address[0]) == 6;
}

// Format a little-endian address as "aa:bb:cc:dd:ee:ff" (most significant byte first), as EspBle does
String formatAddress(uint8_t const *address) {
  char text[18];
  snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[5], address[4], address[3], address[2],
           address[1], address[0]);
  return String(text);
}
