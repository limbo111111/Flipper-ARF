#include <furi_hal_ibutton.h>
#include <furi_hal_resources.h>

void furi_hal_ibutton_pin_configure(void) {
    furi_hal_gpio_write(&gpio_ibutton, true);
    furi_hal_gpio_init(&gpio_ibutton, GpioModeOutputOpenDrain, GpioPullNo, GpioSpeedLow);
}

void furi_hal_ibutton_pin_reset(void) {
    furi_hal_gpio_write(&gpio_ibutton, true);
    furi_hal_gpio_init(&gpio_ibutton, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

void furi_hal_ibutton_pin_write(const bool state) {
    furi_hal_gpio_write(&gpio_ibutton, state);
}
