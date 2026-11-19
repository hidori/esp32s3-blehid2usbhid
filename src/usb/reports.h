// Reports waiting to be sent to the host: the boot keyboard reports and the media keys. Notifications from the keyboard
// can arrive back to back, before the host has taken the previous report; dropping one would lose a key or leave a key
// pressed

#pragma once

#include <stdint.h>

// Queue a boot keyboard report (the modifiers and 6 keys), unless it is the state the host will already have
void queueKeyboardReport(uint8_t modifiers, uint8_t const *keys);

// Queue a media key (its usage, 0 for none), unless it is the one the host will already have
void queueMediaKey(uint16_t usage);

// The media key that the host will have when the queue has been sent
uint16_t lastQueuedMediaKey();

// Queue the release of every key and media key
void queueReleaseAll();

// Send the queued reports one at a time whenever the host can take one (called in loop())
void sendReports();
