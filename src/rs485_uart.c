#include "rs485_uart.h"
#include "rs485_store.h"
#include "ble_transport.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>

#define RS485_UART_NODE  DT_ALIAS(rs485_uart)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

/* How often the ISR ring buffer is drained.
 * At 115200 baud ~12 bytes arrive per ms; 512 bytes of ring buffer gives
 * >40 ms of headroom before any byte is lost, so 1 ms is conservative.
 */
#define DRAIN_PERIOD_MS 1

/* Ring buffer between the UART ISR and the drain work item. */
#define RING_BUF_CAPACITY 512
RING_BUF_DECLARE(rs485_ring, RING_BUF_CAPACITY);

static const struct device *uart_dev = DEVICE_DT_GET(RS485_UART_NODE);
static const struct gpio_dt_spec de_gpio =
    GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, rs485_de_gpios);

/* ---------------------------------------------------------------------------
 * Periodic drain: ring buffer → rs485_store + optional BLE streaming
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
    }
}

static K_WORK_DEFINE(drain_work, drain_work_handler);

static void drain_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&drain_work);
}

static K_TIMER_DEFINE(drain_timer, drain_timer_handler, NULL);

/* ---------------------------------------------------------------------------
 * UART interrupt callback — ISR context
 * ---------------------------------------------------------------------------
 */
static void uart_irq_callback(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (!uart_irq_rx_ready(dev)) {
            continue;
        }
        uint8_t byte;
        while (uart_fifo_read(dev, &byte, 1) == 1) {
            ring_buf_put(&rs485_ring, &byte, 1);
        }
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

    uart_irq_callback_user_data_set(uart_dev, uart_irq_callback, NULL);
    uart_irq_rx_enable(uart_dev);

    k_timer_start(&drain_timer, K_MSEC(DRAIN_PERIOD_MS), K_MSEC(DRAIN_PERIOD_MS));

    printk("rs485: UART receive started\n");
    return 0;
}
