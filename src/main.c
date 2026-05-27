#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "ble_transport.h"

static const struct gpio_dt_spec green_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
    printk("flexitMC3 starting.\n");

    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);

    if (ble_transport_init() < 0) {
        printk("error: BLE transport init failed\n");
        return -1;
    }

    while (true) {
        gpio_pin_toggle_dt(&green_led);
        ble_transport_ensure_advertising();
        k_msleep(1000);
    }

    return 0;
}
