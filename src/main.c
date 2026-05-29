#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "ble_transport.h"
#include "flexit_slave.h"
#include "rs485_uart.h"
#include "zigbee_ep.h"

static const struct gpio_dt_spec green_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
    printk("flexitMC3 starting.\n");

    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);

    if (rs485_uart_init() < 0) {
        printk("warning: RS485 UART init failed — continuing without RS485 receive\n");
    }

    (void)flexit_slave_init();

    if (ble_transport_init() < 0) {
        printk("error: BLE transport init failed\n");
        return -1;
    }

    if (zigbee_ep_init() < 0) {
        printk("warning: Zigbee init failed — continuing with BLE only\n");
    }

    while (true) {
        gpio_pin_toggle_dt(&green_led);
        ble_transport_ensure_advertising();
        k_msleep(1000);
    }

    return 0;
}
