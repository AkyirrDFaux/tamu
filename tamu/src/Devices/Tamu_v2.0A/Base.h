#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "esp_timer.h"
const gpio_num_t LED_NOTIFICATION_PIN = GPIO_NUM_2;

#define VOLTAGE (3.3)

// Returns the current uptime in milliseconds.
uint32_t Now()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Blocks the task for `ms` milliseconds.
void Sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Busy-waits for `us` microseconds (no task yield).
void SleepMicro(uint32_t us)
{
    esp_rom_delay_us(us);
}

// Returns the total amount of DRAM available on the device.
int32_t GetRAM()
{
    // Total DRAM available
    return (int32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

// Returns the current amount of free heap memory.
int32_t GetFreeRAM()
{
    // Current free heap
    return (int32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

// Configures `Pin` as a GPIO output.
void PinModeOutput(gpio_num_t Pin)
{
    gpio_reset_pin(Pin);
    gpio_set_direction(Pin, GPIO_MODE_OUTPUT);
}

// Configures `Pin` as a GPIO input.
void PinModeInput(gpio_num_t Pin)
{
    gpio_reset_pin(Pin);
    gpio_set_direction(Pin, GPIO_MODE_INPUT);
}

// Configures `Pin` as a GPIO input with an internal pull-down resistor.
void PinModeInputPullDown(gpio_num_t Pin)
{
    gpio_reset_pin(Pin);
    gpio_set_direction(Pin, GPIO_MODE_INPUT);

    gpio_pullup_dis(Pin);
    gpio_pulldown_en(Pin);
}

// Reads the current level of `Pin`.
bool PinRead(gpio_num_t Pin)
{
    return gpio_get_level(Pin);
}

// Sets `Pin` to a high level.
void PinHigh(gpio_num_t Pin)
{
    gpio_set_level(Pin, 1);
}

// Sets `Pin` to a low level.
void PinLow(gpio_num_t Pin)
{
    gpio_set_level(Pin, 0);
}