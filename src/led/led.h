// Status LED: the RGB LED that shows the state of the whole bridge (see SPEC.md)

#pragma once

// Turn the LED off at the start
void setupLed();

// Show the state of the bridge, lit or blinking (called in loop())
void updateLed();
