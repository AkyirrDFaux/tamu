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

// Sends `Data` over the RS-485 bus with CSMA/CD collision avoidance and verifies the transmitted frame via echo-check; retries up to RS485_RETRIES times.
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

    uint8_t rx_buffer[256 + 13];

    for (int attempt = 0; attempt < RS485_RETRIES; attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485_WaitForSilence();

        // Prepare bus for transmission
        gpio_set_level(RS485_EN_PIN, 1);
        uart_flush_input(UART_NUM_1);
        
        // Write the finalized start byte and struct to the bus
        uart_write_bytes(UART_NUM_1, (const char*)tx_buffer, total_tx_size);
        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
        
        // Switch to RX mode to listen for our own transmission (Echo-Check)
        gpio_set_level(RS485_EN_PIN, 0);
        
        // Read response
        int bytes_read = uart_read_bytes(UART_NUM_1, rx_buffer, total_tx_size, pdMS_TO_TICKS(50));
        
        // Verification: Validate that the hardware looped back the exact frame
        if (bytes_read == (int)total_tx_size && memcmp(tx_buffer, rx_buffer, total_tx_size) == 0) {
            ESP_LOGI("RS485", "Transmission successful on attempt %d", attempt + 1);
            return true;
        }

        // Collision or failed echo: back off with a fresh random delay
        uint32_t backoff_ms = (RawRand() % 16) + 1;
        ESP_LOGW("RS485", "Attempt %d failed (collision?), backoff %lums", attempt + 1, (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
    
    ESP_LOGE("RS485", "Transmission failed after %d attempts", RS485_RETRIES);
    return false;
}

/**
 * @brief Receives and validates a full PacketFrame.
 * @param Data Pointer to the struct where the packet will be stored.
 * @return Total bytes read if successful, 0 if failed or incomplete.
 */
int ReceivePacket(PacketFrame *Data) {
    if (!Data) return 0;

    uint8_t sync;
    // 1. Synchronize: Find the Start Code (0xAA)
    if (uart_read_bytes(UART_NUM_1, &sync, 1, 0) != 1 || sync != 0xAA) {
        return 0;
    }
    
    // 2. Read the Packet Header (12 bytes: crc8 up to srv_src)
    uint8_t *header_ptr = (uint8_t*)Data;
    if (uart_read_bytes(UART_NUM_1, header_ptr, 12, pdMS_TO_TICKS(10)) != 12) {
        return 0;
    }
    
    // 4. Read the Payload
    if (Data->payload_len > 0) {
        if (uart_read_bytes(UART_NUM_1, Data->payload, Data->payload_len, pdMS_TO_TICKS(20)) != Data->payload_len) {
            return 0;
        }
    }
    
    // 5. Validate CRC
    uint16_t calc_crc_len = 11 + Data->payload_len;
    uint8_t calc_crc = Crc8(&Data->flags, calc_crc_len);
    
    if (calc_crc != Data->crc8) {
        ESP_LOGE("RS485", "CRC Mismatch! Expected 0x%02X, Got 0x%02X", Data->crc8, calc_crc);
        ESP_LOG_BUFFER_HEX("RS485_FRAME", (uint8_t*)Data, 12 + Data->payload_len); 
        return 0; 
    }
    
    return (int)(1 + 12 + Data->payload_len); // Return total bytes read (including start byte)
}