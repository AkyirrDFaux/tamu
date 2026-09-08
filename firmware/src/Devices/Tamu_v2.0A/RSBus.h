#include "driver/uart.h"
#include "soc/uart_struct.h" // UART1 register struct (rxfifo_full_thrhd)
#include <string.h>
#include "Core/Functions/Bus.h"

#define TXD_PIN (GPIO_NUM_21) // Change to your TX pin
#define RXD_PIN (GPIO_NUM_20) // Change to your RX pin
#define RS485_EN_PIN (GPIO_NUM_9)

// Configures UART1 as the RS-485 transceiver (TX/RX pins, baud rate, driver install, enable pin).
void SetupRS485()
{
    uart_config_t uart_config = {
        .baud_rate = 460800,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT, // Explicitly set the clock source
        .flags = {},
    };

    // Check for errors in configuration
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Check if the driver is already installed before trying to install it again
    if (!uart_is_driver_installed(UART_NUM_1))
    {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0));
    }

    // The RX ISR only fired as the 128-byte hardware FIFO neared full, so a few ms of the
    // WS2812 LED bit-bang (interrupts masked, see LED.h) could overflow it and drop large
    // RS-485 frames. Trigger the ISR at a small FIFO level so it drains continuously and
    // the FIFO can never fill during an LED chunk.
    UART1.conf1.rxfifo_full_thrhd = 4;

    gpio_reset_pin(RS485_EN_PIN); // Ensure the pin is reset before setting direction
    gpio_set_direction(RS485_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RS485_EN_PIN, 0);
}

#define RS485_RETRIES 3
#define RS485_RETRY_DELAY_MS 20

// CSMA/CD timings at 460800 baud (1 byte = ~21.7us)
#define RS485_BYTE_TIME_US     22
#define RS485_SILENCE_BYTES    8

// Wait for the line to be silent for 8 bytes + Priority/8 + random 0-3 bytes.
// Any received byte resets the silence counter (someone else is transmitting).
// Bounded to RS485_SILENCE_TIMEOUT_MS so a continuously-busy or shorted bus
// cannot wedge the caller forever.
#define RS485_SILENCE_TIMEOUT_MS 100

static void RS485_WaitForSilence(uint8_t priority)
{
    int64_t idle_since = esp_timer_get_time();
    uint8_t prio = priority;
    uint32_t silence_us = (RS485_SILENCE_BYTES + (prio/8) + (RawRand() % 4)) * RS485_BYTE_TIME_US;
    int64_t wait_start = idle_since;

    for (;;)
    {
        size_t len = 0;
        uart_get_buffered_data_len(UART_NUM_1, &len);
        if (len > 0)
        {
            // Bus activity: drain and restart the silence window
            uint8_t drain[32];
            while (len > 0)
            {
                size_t chunk = len > sizeof(drain) ? sizeof(drain) : len;
                uart_read_bytes(UART_NUM_1, drain, chunk, 0);
                len -= chunk;
                size_t avail = 0;
                uart_get_buffered_data_len(UART_NUM_1, &avail);
                if (avail == 0) break;
                len = avail;
            }
            idle_since = esp_timer_get_time();
            continue;
        }
        if ((esp_timer_get_time() - idle_since) >= (int64_t)silence_us)
            return;
        if ((esp_timer_get_time() - wait_start) >= RS485_SILENCE_TIMEOUT_MS * 1000LL)
            return; // Give up: transmit into the best window we had
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Sends `Data` over the RS-485 bus with CSMA/CD collision avoidance. Per Docs/RSBus.md the
// sent data is verified WHILE sending: frames go out in small chunks and each chunk's echo
// is compared as it returns, so a collision aborts mid-frame (only the queued remainder,
// <= one chunk, still leaves the wire) instead of after the whole frame. Unbounded retry
// count RS485_RETRIES with random backoff.
bool SendAndVerifyPacket(const PacketFrame &Data)
{
    // 1. Create a working copy
    PacketFrame tx_frame = Data;

    // 2. Finalize CRC (Calculated over all fields starting at flags)
    uint16_t crc_len = 11 + PayloadBytes(tx_frame);
    tx_frame.crc8 = Crc8(&tx_frame.flags, crc_len);

    // 3. Prepare for transmission (12 bytes header + payload bytes)
    size_t packet_size = 12 + PayloadBytes(tx_frame);
    size_t total_tx_size = 1 + packet_size; // Start byte (0xAA) + packet_size
    // Start byte + header + payload; payload_len is a wire byte in 4-byte units
    // (max 29 units = 116 bytes), so the largest frame is 129 bytes.
    static_assert(MAX_PAYLOAD_SIZE <= 116, "RSBus TX buffer assumes 116-byte payload");
    uint8_t tx_buffer[140];
    tx_buffer[0] = 0xAA;
    memcpy(&tx_buffer[1], &tx_frame, packet_size);

    const size_t CHUNK = 16;               // ~1.4 ms of line time per chunk
    uint8_t rx_chunk[CHUNK];

    for (int attempt = 0; attempt < RS485_RETRIES; attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485_WaitForSilence(Data.priority);

        // Prepare bus for transmission
        gpio_set_level(RS485_EN_PIN, 1);
        uart_flush_input(UART_NUM_1);

        bool collided = false;
        size_t verified = 0;

        for (size_t off = 0; off < total_tx_size; off += CHUNK)
        {
            size_t n = (total_tx_size - off < CHUNK) ? total_tx_size - off : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(tx_buffer + off), n);

            // The echo of this chunk must come back intact while we keep sending.
            int got = uart_read_bytes(UART_NUM_1, rx_chunk, n, pdMS_TO_TICKS(20));
            if (got != (int)n || memcmp(tx_buffer + off, rx_chunk, n) != 0)
            {
                collided = true;
                break;
            }
            verified += n;
        }

        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));

        // Switch back to RX mode
        gpio_set_level(RS485_EN_PIN, 0);

        if (!collided && verified == total_tx_size)
        {
            // NOTE: no ESP_LOG here - the log goes to the USB console, and in USB APP
            // mode that byte stream belongs to the attached app (text would corrupt it).
            return true;
        }

        // Collision or failed echo: back off with a fresh random delay
        uint32_t backoff_ms = (RawRand() % 16) + 1;
        if (!AppConnected) // diagnostics only when no app is attached to the console
        {
            ESP_LOGW("RS485", "Attempt %d failed (%s), backoff %lums", attempt + 1,
                     collided ? "collision" : "echo incomplete", (unsigned long)backoff_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }

    if (!AppConnected)
    {
        ESP_LOGE("RS485", "Transmission failed after %d attempts", RS485_RETRIES);
    }
    return false;
}

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the ESP32 UART driver.
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &b) -> bool {
        return uart_read_bytes(UART_NUM_1, &b, 1, 0) == 1;
    });
}