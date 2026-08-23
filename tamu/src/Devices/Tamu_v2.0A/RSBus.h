#include "driver/uart.h"
#include <string.h>

#define TXD_PIN (GPIO_NUM_21) // Change to your TX pin
#define RXD_PIN (GPIO_NUM_20) // Change to your RX pin
#define RS485_EN_PIN (GPIO_NUM_9)

// Configures UART1 as the RS-485 transceiver (TX/RX pins, baud rate, driver install, enable pin).
void SetupRS485()
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
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

    gpio_reset_pin(RS485_EN_PIN); // Ensure the pin is reset before setting direction
    gpio_set_direction(RS485_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RS485_EN_PIN, 0);
}

#define RS485_RETRIES 3
#define RS485_RETRY_DELAY_MS 20

// CSMA/CD timings at 115200 baud (1 byte = ~86.8us)
#define RS485_BYTE_TIME_US     87
#define RS485_SILENCE_BYTES    8

// Wait for the line to be silent for 8 bytes + a random 0-7 byte backoff.
// Any received byte resets the silence counter (someone else is transmitting).
// Bounded to RS485_SILENCE_TIMEOUT_MS so a continuously-busy or shorted bus
// cannot wedge the caller forever.
#define RS485_SILENCE_TIMEOUT_MS 100

static void RS485_WaitForSilence()
{
    int64_t idle_since = esp_timer_get_time();
    uint32_t silence_us = (RS485_SILENCE_BYTES + (RawRand() % 8)) * RS485_BYTE_TIME_US;
    int64_t wait_start = idle_since;

    for (;;)
    {
        int len = 0;
        uart_get_buffered_data_len(UART_NUM_1, (size_t *)&len);
        if (len > 0)
        {
            // Bus activity: drain and restart the silence window
            uint8_t drain[32];
            while (len > 0)
            {
                int chunk = len > (int)sizeof(drain) ? (int)sizeof(drain) : len;
                uart_read_bytes(UART_NUM_1, drain, chunk, 0);
                len -= chunk;
                int avail = 0;
                uart_get_buffered_data_len(UART_NUM_1, (size_t *)&avail);
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
    uint16_t crc_len = 11 + tx_frame.payload_len;
    tx_frame.crc8 = Crc8(&tx_frame.flags, crc_len);

    // 3. Prepare for transmission (12 bytes header + payload_len)
    size_t packet_size = 12 + tx_frame.payload_len;
    size_t total_tx_size = 1 + packet_size; // Start byte (0xAA) + packet_size
    uint8_t tx_buffer[256 + 13];
    tx_buffer[0] = 0xAA;
    memcpy(&tx_buffer[1], &tx_frame, packet_size);

    const size_t CHUNK = 16;               // ~1.4 ms of line time per chunk
    uint8_t rx_chunk[CHUNK];

    for (int attempt = 0; attempt < RS485_RETRIES; attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485_WaitForSilence();

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

/**
 * @brief Receives and validates a full PacketFrame.
 * @param Data Pointer to the struct where the packet will be stored. Assembly writes
 *             directly into it, so the caller must pass the same buffer every call
 *             (ProcessBus uses a single static frame).
 * @return Total bytes read if successful (including start byte), 0 while incomplete.
 *
 * The assembly state persists across calls: a frame split across two ProcessBus()
 * polls continues where it left off instead of being consumed and discarded (bytes
 * are pulled from the UART driver's RX buffer one at a time). If a sender aborts
 * mid-frame, the stale stage consumes following bytes until length/CRC checks fail
 * and the machine falls back to sync-scanning.
 */
static int RxValidate(PacketFrame *Data)
{
    // CRC covers everything from flags through the end of the payload.
    if (Crc8(&Data->flags, (uint16_t)(11 + Data->payload_len)) != Data->crc8)
        return 0; // corrupted: keep scanning for the next 0xAA
    return (int)(1 + 12 + Data->payload_len);
}

int ReceivePacket(PacketFrame *Data)
{
    if (!Data) return 0;

    enum RxStage : uint8_t { RX_SYNC, RX_HEADER, RX_PAYLOAD };
    static uint8_t stage = RX_SYNC;
    static uint16_t got = 0; // bytes of the current stage stored so far

    for (;;)
    {
        uint8_t b = 0;
        if (uart_read_bytes(UART_NUM_1, &b, 1, 0) != 1)
            break; // driver RX buffer drained

        switch (stage)
        {
        case RX_SYNC:
            if (b == 0xAA)
            {
                stage = RX_HEADER;
                got = 0;
            }
            // else: inter-frame garbage, skip
            break;

        case RX_HEADER:
            ((uint8_t *)Data)[got++] = b;
            if (got < 12)
                break;

            // Header complete: payload_len is a single byte (<=255) and the payload
            // buffer holds 256, so no length guard is needed - proceed to the payload
            // stage (CRC validates the frame on completion).
            stage = RX_PAYLOAD;
            got = 0;
            if (Data->payload_len == 0)
            {
                // Zero-payload frames finish here.
                stage = RX_SYNC;
                int total = RxValidate(Data);
                if (total > 0) return total;
            }
            break;

        case RX_PAYLOAD:
            Data->payload[got++] = b;
            if (got >= Data->payload_len)
            {
                stage = RX_SYNC;
                got = 0;
                int total = RxValidate(Data);
                if (total > 0) return total;
            }
            break;
        }
    }
    return 0; // incomplete: more bytes may arrive later
}