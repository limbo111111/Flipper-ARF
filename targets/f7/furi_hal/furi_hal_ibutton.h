/**
 * @file furi_hal_ibutton.h
 * iButton pin control API (used by RFID HAL for shared GPIO)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the pin to normal mode (open collector), and sets it to float
 */
void furi_hal_ibutton_pin_configure(void);

/**
 * Sets the pin to analog mode, and sets it to float
 */
void furi_hal_ibutton_pin_reset(void);

/**
 * iButton write pin
 * @param state true / false
 */
void furi_hal_ibutton_pin_write(const bool state);

#ifdef __cplusplus
}
#endif
