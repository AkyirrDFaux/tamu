#include "driver/uart.h"
#include "soc/uart_struct.h" // UART1 register struct (rxfifo_full_thrhd)
#include <string.h>
#include "Core/Functions/Bus.h"
#include "Core/Functions/Bootloader.h"

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

// Shared CSMA/CD silence wait (Core/Functions/Bus.h): drain the UART buffer, count
// silence in esp_timer microseconds, sleep 1 ms between polls (the Tamu has a scheduler).
static void RS485_WaitForSilence(uint8_t priority)
{
    RS485_WaitForSilence(priority,
        [](void) -> bool {
            size_t len = 0;
            uart_get_buffered_data_len(UART_NUM_1, &len);
            if (len == 0) return false;
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
            return true;
        },
        [](void) -> uint32_t { return (uint32_t)esp_timer_get_time(); },
        [](void) { vTaskDelay(pdMS_TO_TICKS(1)); });
}

// Sends `Data` over the RS-485 bus with CSMA/CD collision avoidance. Per Docs/RSBus and Packets.md the
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

    // 3. Prepare for transmission: the frame is header + payload bytes, max 128 total.
    // The 0xAA sync byte is sent separately (not part of the frame).
    size_t packet_size = 12 + PayloadBytes(tx_frame);
    static_assert(MAX_PAYLOAD_SIZE <= 116, "RSBus TX buffer assumes a 116-byte payload");
    uint8_t tx_buffer[128];
    memcpy(tx_buffer, &tx_frame, packet_size);

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

        // Sync byte first; its echo is verified too.
        uart_write_bytes(UART_NUM_1, "\xAA", 1);
        {
            int got = uart_read_bytes(UART_NUM_1, rx_chunk, 1, pdMS_TO_TICKS(20));
            if (got != 1 || rx_chunk[0] != 0xAA)
                collided = true;
        }

        for (size_t off = 0; !collided && off < packet_size; off += CHUNK)
        {
            size_t n = (packet_size - off < CHUNK) ? packet_size - off : CHUNK;
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

        if (!collided && verified == packet_size)
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

// Raw bootloader send (Docs/Services/Bootloader.md): the frame is NOT a PacketFrame and
// carries no 0xAA sync, so normal nodes (which wait for 0xAA) ignore it while a bootloader
// waiting for 0xCA receives it. Same CSMA/echo discipline as SendAndVerifyPacket.
bool RS485_SendRaw(const uint8_t *data, size_t len)
{
    const size_t CHUNK = 16; // ~1.4 ms of line time per chunk
    uint8_t rx_chunk[CHUNK];

    for (int attempt = 0; attempt < RS485_RETRIES; attempt++) {
        RS485_WaitForSilence(0); // bootloader traffic is the highest priority
        gpio_set_level(RS485_EN_PIN, 1);
        uart_flush_input(UART_NUM_1);

        bool collided = false;
        size_t verified = 0;
        for (size_t off = 0; !collided && off < len; off += CHUNK) {
            size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(data + off), n);
            int got = uart_read_bytes(UART_NUM_1, rx_chunk, n, pdMS_TO_TICKS(20));
            if (got != (int)n || memcmp(data + off, rx_chunk, n) != 0) {
                collided = true;
                break;
            }
            verified += n;
        }
        // Release TX-enable as soon as the last bit is out. uart_wait_tx_done() wakes on a
        // FreeRTOS notification (~a tick of latency), and the bootloader replies fast enough
        // to collide with the line while we would still be driving it; poll the UART status
        // directly so the standard silence window is enough.
        while (UART1.status.txfifo_cnt != 0 || UART1.fsm_status.st_utx_out != 0) {}
        gpio_set_level(RS485_EN_PIN, 0);

        if (!collided && verified == len)
            return true;
        vTaskDelay(pdMS_TO_TICKS((RawRand() % 16) + 1));
    }
    return false;
}

// Receives one raw bootloader response frame (0xCA ... 0xBC). Scans past non-frame bytes,
// derives the length from the control byte and validates with the codec. Returns the frame
// length, or 0 on timeout/invalid. Reads the UART directly: the caller is the synchronous
// Device 0021 handler, so the normal assembler is not running concurrently.
int RS485_ReceiveRaw(uint8_t *out, size_t cap, uint32_t timeout_ms)
{
    uint32_t start = TimeFromBoot();
    size_t got = 0;
    bool in_frame = false;

    while ((uint32_t)(TimeFromBoot() - start) < timeout_ms) {
        uint8_t b;
        if (uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(5)) != 1)
            continue;
        if (!in_frame) {
            if (b == Bootloader::START) { in_frame = true; got = 0; out[got++] = b; }
            continue;
        }
        if (got < cap)
            out[got++] = b;
        if (got >= 2) {
            uint16_t need = Bootloader::FrameSize((uint8_t)(out[1] & 0x03));
            if (need == 0 || need > cap) { in_frame = false; got = 0; continue; } // bad control
            if (got >= need) {
                if (Bootloader::Decode(out, need)) return (int)need;
                in_frame = false; got = 0; // corrupt frame: resync on the next 0xCA
            }
        }
    }
    return 0;
}

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the ESP32 UART driver.
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &b) -> bool {
        return uart_read_bytes(UART_NUM_1, &b, 1, 0) == 1;
    });
}