// USB device: its configuration, what the host sends to it (the lamps, and the requests from the settings page), and
// the reports and the responses sent to the host

#pragma once

#include <stddef.h>
#include <stdint.h>

// Size of a request and of a response of the settings page, without the report ID (see SPEC.md)
const uint8_t SETTINGS_REPORT_SIZE = 63;

// Set up the USB device: a keyboard, and the settings HID with the consumer control for the media keys
void setupUsb();

// Lamps (Caps Lock etc.) that the host has, as in a boot keyboard output report: Num Lock (bit 0) to Kana (bit 4)
uint8_t hostLamps();

// Take the next request from the settings page (SETTINGS_REPORT_SIZE bytes); false if there is none
bool receiveSettingsRequest(uint8_t *request);

// Send a response to the settings page
void sendSettingsResponse(uint8_t command, uint8_t result, uint8_t const *data, size_t size);

// Wait until the host has taken the response (gives up after about 100 ms)
bool waitSettingsResponseTaken();

// Whether the host can take reports (not while it is suspended, for example)
bool isHostReady();

// Send a boot keyboard report (the modifiers and 6 keys), or a media key (its usage, 0 for none). False if the host
// cannot take it now
bool sendKeyboardReport(uint8_t modifiers, uint8_t *keys);
bool sendMediaKey(uint16_t usage);
