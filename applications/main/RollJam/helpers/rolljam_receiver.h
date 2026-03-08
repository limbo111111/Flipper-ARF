#pragma once

#include "../rolljam.h"

/*
 * Internal CC1101 raw signal capture and transmission.
 *
 * Capture: uses narrow RX bandwidth so the offset jamming
 *          from the external CC1101 is filtered out.
 *
 * The captured raw data is stored as signed int16 values:
 *   positive = high-level duration (microseconds)
 *   negative = low-level duration (microseconds)
 *
 * This matches the Flipper .sub RAW format.
 */

// Start raw capture on internal CC1101
void rolljam_capture_start(RollJamApp* app);

// Stop capture
void rolljam_capture_stop(RollJamApp* app);

// Check if captured signal looks valid (not just noise)
bool rolljam_signal_is_valid(RawSignal* signal);

// Transmit a raw signal via internal CC1101
void rolljam_transmit_signal(RollJamApp* app, RawSignal* signal);

// Save signal to .sub file on SD card
void rolljam_save_signal(RollJamApp* app, RawSignal* signal);
