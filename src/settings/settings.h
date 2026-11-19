// Settings HID protocol: the requests from the settings page and the responses to them (see SPEC.md)

#pragma once

#include <stdint.h>

// Handle a request from the settings page (SETTINGS_REPORT_SIZE bytes)
void handleSettingsRequest(uint8_t const *request);
