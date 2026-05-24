#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
    printk("Hello, World! flexitMC3 starting on XIAO BLE + RS485 expansion board.\n");

    if (!gpio_is_ready_dt(&green_led)) {
        printk("error: green LED GPIO not ready\n");
        return -1;
    }

    if (gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE) < 0) {
        printk("error: failed to configure green LED\n");
        return -1;
    }

    while (true) {
        gpio_pin_toggle_dt(&green_led);
        k_msleep(500);
    }

    return 0;
}
