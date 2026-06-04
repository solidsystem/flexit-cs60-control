#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

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

/* ---------------------------------------------------------------------------
 * Watchdog
 *
 * The diagnosed field failure is a wedged *system workqueue* (the RS485 drain
 * and Zigbee bridge both run there) while the BLE/SMP threads keep answering —
 * so a watchdog fed only from main() would never fire: main() keeps spinning.
 * We run two task-wdt channels backed by the nRF hardware WDT:
 *   - channel 0 (main): fed every tick from the loop below — catches a hung
 *                       main thread.
 *   - channel 1 (wq):   fed by a heartbeat work item on the system workqueue —
 *                       catches the workqueue wedge, which is the actual fault.
 * task_wdt's expiry runs in the kernel-timer ISR (independent of the
 * workqueue), so a starved channel reboots the SoC even when the workqueue is
 * stuck; MCUboot then re-runs the confirmed image. This automates the manual
 * `ble-client reboot` recovery.
 *
 * Periods are generous: a feed merely has to land within them. The wq heartbeat
 * reschedules far more often than its channel period so ordinary TX stalls in
 * the drain path don't false-trip; only a true wedge (stuck forever inside one
 * work item) lets the channel lapse.
 * ---------------------------------------------------------------------------
 */
#define WDT_MAIN_FEED_PERIOD_MS  5000   /* main loop ticks every LED_TICK_MS */
#define WDT_WQ_HEARTBEAT_MS      1000   /* heartbeat reschedule cadence       */
#define WDT_WQ_FEED_PERIOD_MS    8000   /* workqueue stall tolerated before reset */

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_main_chan = -1;
static int wdt_wq_chan   = -1;

/* Breadcrumb left for the next boot to report which channel tripped. __noinit
 * RAM survives the cold reset; validated by a magic to ignore power-on garbage. */
static __noinit volatile uint32_t wdt_trip_marker;
#define WDT_TRIP_MAGIC 0x5744u  /* 'WD' */

static const char *wdt_chan_name(uint32_t ch)
{
    switch (ch) {
    case 0:  return "main loop";
    case 1:  return "system-workqueue heartbeat";
    default: return "?";
    }
}

/* Runs in the task_wdt kernel-timer ISR when a channel starves. Deferred
 * logging can't flush here, so leave a RAM breadcrumb, then cold-reset. */
static void wdt_timeout_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(user_data);
    wdt_trip_marker = WDT_TRIP_MAGIC | ((uint32_t)(channel_id & 0xFF) << 16);
    sys_reboot(SYS_REBOOT_COLD);
}

static void wdt_wq_heartbeat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wdt_wq_heartbeat, wdt_wq_heartbeat_fn);

static void wdt_wq_heartbeat_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    if (wdt_wq_chan >= 0) {
        (void)task_wdt_feed(wdt_wq_chan);
    }
    k_work_reschedule(&wdt_wq_heartbeat, K_MSEC(WDT_WQ_HEARTBEAT_MS));
}

/* Report any watchdog reset from the previous boot, then clear the breadcrumb. */
static void watchdog_report_prior_trip(void)
{
    if ((wdt_trip_marker & 0xFFFFu) == WDT_TRIP_MAGIC) {
        uint32_t ch = (wdt_trip_marker >> 16) & 0xFFu;
        printk("WATCHDOG: previous boot was reset by task-wdt channel %u (%s)\n",
               ch, wdt_chan_name(ch));
    }
    wdt_trip_marker = 0;
}

/* Best-effort: install the two channels and start the workqueue heartbeat.
 * A missing/unready WDT is logged but not fatal — the device still runs. */
static void watchdog_init(void)
{
    if (!device_is_ready(wdt_dev)) {
        printk("warning: watchdog device not ready — running without watchdog\n");
        return;
    }

    int err = task_wdt_init(wdt_dev);
    if (err) {
        printk("warning: task_wdt_init failed: %d — running without watchdog\n", err);
        return;
    }

    /* Add order fixes the channel ids the breadcrumb decodes: 0=main, 1=wq. */
    wdt_main_chan = task_wdt_add(WDT_MAIN_FEED_PERIOD_MS, wdt_timeout_cb, NULL);
    wdt_wq_chan   = task_wdt_add(WDT_WQ_FEED_PERIOD_MS, wdt_timeout_cb, NULL);
    if (wdt_main_chan < 0 || wdt_wq_chan < 0) {
        printk("warning: task_wdt_add failed (main=%d wq=%d)\n",
               wdt_main_chan, wdt_wq_chan);
        return;
    }

    k_work_reschedule(&wdt_wq_heartbeat, K_MSEC(WDT_WQ_HEARTBEAT_MS));
    printk("watchdog: task_wdt up (main=%u ms, workqueue=%u ms)\n",
           WDT_MAIN_FEED_PERIOD_MS, WDT_WQ_FEED_PERIOD_MS);
}

int main(void)
{
    printk("flexit-cs60-control starting..\n");

    /* Surface a watchdog reset from the previous boot before anything else, so
     * a field hang leaves a fingerprint in the console/NUS log. */
    watchdog_report_prior_trip();

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
    /* Start the watchdog only now that subsystem init is done, so the lengthy
     * one-time init (BLE keygen etc.) can't trip a freshly-added channel. */
    watchdog_init();

    for (uint32_t tick = 0;; tick++) {
        if (wdt_main_chan >= 0) {
            (void)task_wdt_feed(wdt_main_chan);
        }

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
