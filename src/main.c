#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "ble_transport.h"
#include "flexit_bridge.h"
#include "flexit_slave.h"
#include "panel_mirror.h"
#include "rs485_uart.h"
#include "zigbee_ep.h"

static const struct gpio_dt_spec red_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec green_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec blue_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

/* Status-LED timing. The loop ticks at the fast-blink period; the slower
 * cadences are multiples of it. */
#define LED_TICK_MS          100   /* loop period == fast (joining) blink step */
#define LED_SLOW_BLINK_TICKS 10    /* idle blink: toggle every 1 s            */
#define RED_BLINK_TICKS      5     /* RS485-fault blink: toggle every 500 ms   */
#define ADV_ENSURE_TICKS     10    /* re-check advertising about once a second */

/* The CS60 broadcasts its FC10 status frame continuously (well under a second
 * apart). Treat the RS485 link as down if no valid frame has arrived for this
 * long — generous enough to ride out occasional CRC drops, short enough that a
 * pulled cable or wrong wiring shows on the LED within a few seconds. */
#define RS485_CS60_STALE_MS  5000

int main(void)
{
    printk("flexit-cs60-control starting.\n");

    gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);

    /* Latch any init failure so the red LED can flag it below. Each subsystem
     * still runs best-effort (we don't bail out), so a single failure doesn't
     * take down the others — the LED is the operator's signal that something
     * came up degraded. */
    bool init_failed = false;

    if (rs485_uart_init() < 0) {
        printk("warning: RS485 UART init failed — continuing without RS485 receive\n");
        init_failed = true;
    }

    if (flexit_slave_init() < 0) {
        printk("warning: Flexit Modbus slave init failed\n");
        init_failed = true;
    }

    if (ble_transport_init() < 0) {
        printk("error: BLE transport init failed\n");
        init_failed = true;
    }

    if (zigbee_ep_init() < 0) {
        printk("warning: Zigbee init failed — continuing with BLE only\n");
        init_failed = true;
    }

    if (flexit_bridge_init() < 0) {
        printk("warning: Flexit bridge init failed\n");
        init_failed = true;
    }


    /* Status LEDs:
     *   red (fault):
     *     - solid on if any subsystem init failed
     *     - else blink (500 ms) if the RS485 link to the CS60 is down
     *       (no recent FC10 frame — miswired/unpaired bus or CS60 absent)
     *     - else off
     *   green (Zigbee):
     *     - Zigbee join window open  -> fast blink (toggle every 100 ms)
     *     - Zigbee joined            -> solid on
     *     - otherwise                -> slow blink (toggle every 1 s)
     *   blue (BLE):
     *     - solid on while a central is connected, else off
     */
    for (uint32_t tick = 0;; tick++) {
        /* Red: init failure is latched-solid and outranks the link-down blink. */
        if (init_failed) {
            gpio_pin_set_dt(&red_led, 1);
        } else if (!panel_mirror_cs60_link_up(RS485_CS60_STALE_MS)) {
            if (tick % RED_BLINK_TICKS == 0) {
                gpio_pin_toggle_dt(&red_led);
            }
        } else {
            gpio_pin_set_dt(&red_led, 0);
        }

        switch (zigbee_ep_net_state()) {
        case ZIGBEE_NET_JOINING:
            gpio_pin_toggle_dt(&green_led);
            break;
        case ZIGBEE_NET_JOINED:
            gpio_pin_set_dt(&green_led, 1);
            break;
        case ZIGBEE_NET_IDLE:
        default:
            if (tick % LED_SLOW_BLINK_TICKS == 0) {
                gpio_pin_toggle_dt(&green_led);
            }
            break;
        }

        /* Blue LED: solid on while a BLE central is connected, else off. */
        gpio_pin_set_dt(&blue_led, ble_transport_is_connected() ? 1 : 0);

        if (tick % ADV_ENSURE_TICKS == 0) {
            ble_transport_ensure_advertising();
        }

        k_msleep(LED_TICK_MS);
    }

    return 0;
}
