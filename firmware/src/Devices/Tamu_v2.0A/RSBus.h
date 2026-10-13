#include "driver/uart.h"
#include "soc/uart_struct.h" // UART1 register struct (rxfifo_full_thrhd)
#include <string.h>
#include "Core/Functions/Bus.h"
#include "Core/Functions/Bootloader.h"

#define TXD_PIN (GPIO_NUM_21) // RS-485 transceiver TX (to DI)
#define RXD_PIN (GPIO_NUM_20) // RS-485 transceiver RX (from RO)
#define RS485_EN_PIN (GPIO_NUM_9)

// Configures UART1 as the RS-485 transceiver (TX/RX pins, baud rate, driver install, enable pin).
void SetupRS485()
{
    uart_config_t UartConfig = {
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
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &UartConfig));
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
static void RS485WaitForSilence(uint8_t Priority)
{
    RS485WaitForSilence(Priority,
        [](void) -> bool {
            size_t Len = 0;
            uart_get_buffered_data_len(UART_NUM_1, &Len);
            if (Len == 0) return false;
            // Bus activity: drain and restart the silence window
            uint8_t Drain[32];
            while (Len > 0)
            {
                size_t Chunk = Len > sizeof(Drain) ? sizeof(Drain) : Len;
                uart_read_bytes(UART_NUM_1, Drain, Chunk, 0);
                Len -= Chunk;
                size_t Avail = 0;
                uart_get_buffered_data_len(UART_NUM_1, &Avail);
                if (Avail == 0) break;
                Len = Avail;
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
    // 1. A working copy, deliberately LOCAL: it frees the caller's frame as soon as we return,
    // and it lets the CRC be finalised here (the input is const). It must not reuse the shared
    // TxFrame, whose name it used to shadow.
    PacketFrame Work = Data;

    // 2. Finalize CRC (Calculated over all fields starting at flags)
    uint16_t CrcLen = 11 + PayloadBytes(Work);
    Work.Crc8 = Crc8(&Work.Flags, CrcLen);

    // 3. Prepare for transmission: the frame is header + payload bytes, max 128 total.
    // The 0xAA sync byte is sent separately (not part of the frame).
    size_t PacketSize = 12 + PayloadBytes(Work);
    static_assert(MAX_PAYLOAD_SIZE <= 116, "RSBus TX buffer assumes a 116-byte Payload");
    uint8_t TxBuffer[128];
    memcpy(TxBuffer, &Work, PacketSize);

    const size_t CHUNK = 16;               // ~1.4 ms of line time per chunk
    uint8_t RxChunk[CHUNK];

    for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485WaitForSilence(Data.Priority);

        // Prepare bus for transmission
        gpio_set_level(RS485_EN_PIN, 1);
        uart_flush_input(UART_NUM_1);

        bool Collided = false;
        size_t Verified = 0;

        // Sync byte first; its echo is verified too.
        uart_write_bytes(UART_NUM_1, "\xAA", 1);
        {
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, 1, pdMS_TO_TICKS(20));
            if (Got != 1 || RxChunk[0] != 0xAA)
                Collided = true;
        }

        for (size_t Off = 0; !Collided && Off < PacketSize; Off += CHUNK)
        {
            size_t ChunkLen = (PacketSize - Off < CHUNK) ? PacketSize - Off : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(TxBuffer + Off), ChunkLen);

            // The echo of this chunk must come back intact while we keep sending.
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, ChunkLen, pdMS_TO_TICKS(20));
            if (Got != (int)ChunkLen || memcmp(TxBuffer + Off, RxChunk, ChunkLen) != 0)
            {
                Collided = true;
                break;
            }
            Verified += ChunkLen;
        }

        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));

        // Switch back to RX mode
        gpio_set_level(RS485_EN_PIN, 0);

        if (!Collided && Verified == PacketSize)
        {
            // NOTE: no ESP_LOG here - the log goes to the USB console, and in USB APP
            // mode that byte stream belongs to the attached app (text would corrupt it).
            return true;
        }

        // Collision or failed echo: back off with a fresh random delay
        uint32_t BackoffMs = (RawRand() % 16) + 1;
        if (!AppInterface.Connected) // diagnostics only when no app is attached to the console
        {
            ESP_LOGW("RS485", "Attempt %d failed (%s), backoff %lums", Attempt + 1,
                     Collided ? "collision" : "echo incomplete", (unsigned long)BackoffMs);
        }
        vTaskDelay(pdMS_TO_TICKS(BackoffMs));
    }

    if (!AppInterface.Connected)
    {
        ESP_LOGE("RS485", "Transmission failed after %d attempts", RS485_RETRIES);
    }
    return false;
}

// Raw bootloader send (Docs/Services/Bootloader.md): the frame is NOT a PacketFrame and
// carries no 0xAA sync, so normal nodes (which wait for 0xAA) ignore it while a bootloader
// waiting for 0xCA receives it. Same CSMA/echo discipline as SendAndVerifyPacket.
bool RS485SendRaw(const uint8_t *Data, size_t Len)
{
    const size_t CHUNK = 16; // ~1.4 ms of line time per chunk
    uint8_t RxChunk[CHUNK];

    for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++) {
        RS485WaitForSilence(0); // bootloader traffic is the highest priority
        gpio_set_level(RS485_EN_PIN, 1);
        uart_flush_input(UART_NUM_1);

        bool Collided = false;
        size_t Verified = 0;
        for (size_t Off = 0; !Collided && Off < Len; Off += CHUNK) {
            size_t ChunkLen = (Len - Off < CHUNK) ? (Len - Off) : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(Data + Off), ChunkLen);
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, ChunkLen, pdMS_TO_TICKS(20));
            if (Got != (int)ChunkLen || memcmp(Data + Off, RxChunk, ChunkLen) != 0) {
                Collided = true;
                break;
            }
            Verified += ChunkLen;
        }
        // Release TX-enable as soon as the last bit is out. uart_wait_tx_done() wakes on a
        // FreeRTOS notification (~a tick of latency), and the bootloader replies fast enough
        // to collide with the line while we would still be driving it; poll the UART status
        // directly so the standard silence window is enough.
        while (UART1.status.txfifo_cnt != 0 || UART1.fsm_status.st_utx_out != 0) {}
        gpio_set_level(RS485_EN_PIN, 0);

        if (!Collided && Verified == Len)
            return true;
        vTaskDelay(pdMS_TO_TICKS((RawRand() % 16) + 1));
    }
    return false;
}

// While a Device bootloader passthrough is outstanding the Device service installs a sink here:
// the shared reader offers every byte to it first, so a raw 0xCA reply is stored for the
// passthrough instead of being discarded as non-0xAA noise by the packet assembler. One pass
// over the UART therefore serves both packet types. Null when nothing is expected, so the
// normal path is byte-for-byte unchanged.
bool (*BootloaderRxSink)(uint8_t Byte) = nullptr;

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the ESP32 UART driver.
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &Byte) -> bool {
        for (;;) {
            if (uart_read_bytes(UART_NUM_1, &Byte, 1, 0) != 1)
                return false;
            if (BootloaderRxSink && BootloaderRxSink(Byte))
                continue; // consumed by the bootloader passthrough
            return true;
        }
    });
}