#include "rs485_uart.h"
#include "rs485_store.h"
#include "ble_transport.h"
#include "panel_mirror.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>

#define RS485_UART_NODE  DT_ALIAS(rs485_uart)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

/* Two DMA RX buffers. The UARTE driver fills one while we have the
 * other; when one fills (or the bus idles past RX_TIMEOUT_US) we get a
 * UART_RX_RDY event and the driver switches to the other. At 115200
 * baud, 256 B per buffer = ~22 ms before overflow — far more headroom
 * than the per-byte IRQ path (which only tolerates ~1.4 ms of latency
 * before the 16-byte UARTE FIFO overruns).
 */
#define RX_BUF_SIZE 256
static uint8_t rx_buf_a[RX_BUF_SIZE];
static uint8_t rx_buf_b[RX_BUF_SIZE];

/* UART_RX_RDY fires when either the buffer fills or no new byte has
 * arrived for this many microseconds. 1 ms is short enough to keep
 * forwarding latency low, long enough to coalesce typical bursts.
 */
#define RX_TIMEOUT_US 1000

/* Ring buffer hands data off from the async callback (ISR context)
 * to the drain worker (system workqueue). 1 KiB = ~89 ms of data at
 * 115200 baud — survives a brief BLE host stall.
 */
#define RING_BUF_CAPACITY 1024
RING_BUF_DECLARE(rs485_ring, RING_BUF_CAPACITY);

static const struct device *uart_dev = DEVICE_DT_GET(RS485_UART_NODE);
static const struct gpio_dt_spec de_gpio =
    GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, rs485_de_gpios);

/* Which DMA buffer to hand the driver next (0 = rx_buf_a, 1 = rx_buf_b).
 * Only mutated from the async callback; atomic for read consistency.
 */
static atomic_t next_buf;

static atomic_t dropped_bytes;     /* ring-buffer overflow counter */
static atomic_t stop_events;       /* UART_RX_STOPPED count (overrun/framing) */

/* ---------------------------------------------------------------------------
 * Drain worker: ring buffer → rs485_store + optional BLE streaming
 * ---------------------------------------------------------------------------
 */
static void drain_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    uint8_t tmp[128];
    uint32_t n;
    while ((n = ring_buf_get(&rs485_ring, tmp, sizeof(tmp))) > 0) {
        rs485_store_append(tmp, (size_t)n);
        ble_transport_forward_rs485(tmp, (size_t)n);
        panel_mirror_feed(tmp, (size_t)n);
    }
}

static K_WORK_DEFINE(drain_work, drain_work_handler);

/* ---------------------------------------------------------------------------
 * UART async callback — ISR context for the NRFX UARTE driver.
 * ---------------------------------------------------------------------------
 */
static void uart_async_cb(const struct device *dev,
                          struct uart_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case UART_RX_RDY: {
        const uint8_t *src = evt->data.rx.buf + evt->data.rx.offset;
        uint32_t       len = (uint32_t)evt->data.rx.len;
        uint32_t       put = ring_buf_put(&rs485_ring, src, len);
        if (put < len) {
            atomic_add(&dropped_bytes, (atomic_val_t)(len - put));
        }
        k_work_submit(&drain_work);
        break;
    }

    case UART_RX_BUF_REQUEST: {
        /* DMA wants the next buffer to swap to. Hand over whichever
         * one isn't currently being filled.
         */
        int      idx = (int)atomic_get(&next_buf);
        uint8_t *buf = (idx == 0) ? rx_buf_a : rx_buf_b;
        (void)uart_rx_buf_rsp(dev, buf, RX_BUF_SIZE);
        atomic_set(&next_buf, idx ^ 1);
        break;
    }

    case UART_RX_BUF_RELEASED:
        /* Buffer ownership returned — the two static buffers cycle on
         * their own, nothing to do.
         */
        break;

    case UART_RX_DISABLED:
        /* Should only happen on hard error; restart continuous receive. */
        atomic_set(&next_buf, 1);
        (void)uart_rx_enable(dev, rx_buf_a, RX_BUF_SIZE, RX_TIMEOUT_US);
        break;

    case UART_RX_STOPPED:
        atomic_inc(&stop_events);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public init
 * ---------------------------------------------------------------------------
 */
int rs485_uart_init(void)
{
    if (!device_is_ready(uart_dev)) {
        printk("rs485: UART device not ready\n");
        return -ENODEV;
    }

    /* DE/RE LOW → high-Z receive mode. */
    if (device_is_ready(de_gpio.port)) {
        gpio_pin_configure_dt(&de_gpio, GPIO_OUTPUT_INACTIVE);
    }

    int err = uart_callback_set(uart_dev, uart_async_cb, NULL);
    if (err) {
        printk("rs485: uart_callback_set failed: %d\n", err);
        return err;
    }

    atomic_set(&next_buf, 1);  /* first buffer is rx_buf_a; supply b next */
    err = uart_rx_enable(uart_dev, rx_buf_a, RX_BUF_SIZE, RX_TIMEOUT_US);
    if (err) {
        printk("rs485: uart_rx_enable failed: %d\n", err);
        return err;
    }

    printk("rs485: UART async receive started (DMA, 2x %u B buffers)\n",
           RX_BUF_SIZE);
    return 0;
}
