#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include "ble_transport.h"

static const struct gpio_dt_spec green_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec blue_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec rs485_de =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rs485_de_gpios);

static const struct device *const rs485_uart =
    DEVICE_DT_GET(DT_ALIAS(rs485_uart));

#define RX_RING_SIZE 512
RING_BUF_DECLARE(rx_rb, RX_RING_SIZE);

#define BLUE_BLINK_MS    5000
#define BLUE_BLINK_TICK  K_MSEC(100)

static int64_t blue_blink_until_ms;

static void rx_work_handler(struct k_work *work);
static void blue_blink_work_handler(struct k_work *work);

static K_WORK_DEFINE(rx_work, rx_work_handler);
static K_WORK_DELAYABLE_DEFINE(blue_blink_work, blue_blink_work_handler);

static void uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (!uart_irq_rx_ready(dev)) {
            continue;
        }

        uint8_t buf[32];
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            continue;
        }

        ring_buf_put(&rx_rb, buf, n);
        k_work_submit(&rx_work);
    }
}

static void rx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t buf[64];
    uint32_t n;

    while ((n = ring_buf_get(&rx_rb, buf, sizeof(buf))) > 0) {
        printk("RS485 RX (%u):", n);
        for (uint32_t i = 0; i < n; i++) {
            printk(" %02X", buf[i]);
        }
        printk("\n");

        blue_blink_until_ms = k_uptime_get() + BLUE_BLINK_MS;
        k_work_schedule(&blue_blink_work, K_NO_WAIT);
    }
}

static void blue_blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);

    if (k_uptime_get() >= blue_blink_until_ms) {
        gpio_pin_set_dt(&blue_led, 0);
        return;
    }

    gpio_pin_toggle_dt(&blue_led);
    k_work_schedule(dwork, BLUE_BLINK_TICK);
}

int main(void)
{
    printk("flexitMC3 starting.\n");

    if (!gpio_is_ready_dt(&green_led) ||
        !gpio_is_ready_dt(&blue_led) ||
        !gpio_is_ready_dt(&rs485_de)) {
        printk("error: GPIOs not ready\n");
        return -1;
    }

    if (gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure_dt(&rs485_de, GPIO_OUTPUT_INACTIVE) < 0) {
        printk("error: failed to configure GPIOs\n");
        return -1;
    }

    /* DE/RE LOW = SP3485 in receive mode (high-Z driver, RX enabled). */
    gpio_pin_set_dt(&rs485_de, 0);

    if (!device_is_ready(rs485_uart)) {
        printk("error: RS485 UART not ready\n");
        return -1;
    }

    uart_irq_rx_disable(rs485_uart);
    uart_irq_tx_disable(rs485_uart);
    uart_irq_callback_set(rs485_uart, uart_isr);
    uart_irq_rx_enable(rs485_uart);

    printk("Listening on RS485 (D4=RX, D5=TX, D2=DE/RE) @ 115200 8N1...\n");

    if (ble_transport_init() < 0) {
        printk("warn: BLE transport not started\n");
    }

    while (true) {
        gpio_pin_toggle_dt(&green_led);
        k_msleep(500);
    }

    return 0;
}
