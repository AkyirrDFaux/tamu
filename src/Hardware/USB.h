namespace HW
{
#define MAX_USB_DATA 60
#define USB_PACKET_SIZE 64
    uint8_t CRC8(const uint8_t *Data, size_t Length)
    {
        uint8_t CRC8 = 0x00;
        for (size_t i = 0; i < Length; i++)
        {
            CRC8 ^= Data[i];
            for (int j = 0; j < 8; j++)
            {
                if (CRC8 & 0x80)
                    CRC8 = (CRC8 << 1) ^ 0x31;
                else
                    CRC8 <<= 1;
            }
        }
        return CRC8;
    }
#if defined BOARD_Tamu_v2_0
#include "driver/usb_serial_jtag.h"
    void USB_Init()
    {
        usb_serial_jtag_driver_config_t usb_config = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 1024,
        };
        usb_serial_jtag_driver_install(&usb_config);
    }

    void USB_Send(const FlexArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr || !usb_serial_jtag_is_connected())
            return;

        uint32_t offset = 0;
        uint8_t packet[USB_PACKET_SIZE];

        while (offset < Data.Length)
        {
            // Calculate payload size for this packet
            uint8_t to_copy = (uint8_t)((Data.Length - offset > MAX_USB_DATA) ? MAX_USB_DATA : Data.Length - offset);

            // --- PACKET CONSTRUCION ---
            packet[0] = 0xFA;    // Header
            packet[2] = to_copy; // Payload Length
            memcpy(packet + 3, Data.Array + offset, to_copy);
            packet[3 + to_copy] = 0xBF; // Footer

            // CRC8 covers the length byte and the payload
            packet[1] = CRC8(packet + 2, to_copy + 1);

            // Non-blocking write with 10ms timeout
            int sent = usb_serial_jtag_write_bytes(packet, USB_PACKET_SIZE, pdMS_TO_TICKS(10));

            if (sent <= 0)
                break; // Device disconnected or buffer full

            offset += to_copy;
        }
    }

    uint16_t USB_Read(FlexArray &OutBuffer)
    {
        static uint8_t rx_buffer[512];
        static uint32_t rx_ptr = 0;

        // 1. Pull new data from hardware into the static circular-ish buffer
        int len = usb_serial_jtag_read_bytes(rx_buffer + rx_ptr, (sizeof(rx_buffer) - rx_ptr), 0);
        if (len > 0)
            rx_ptr += len;

        uint32_t cursor = 0;

        // 2. Scan through the buffer for a full USB packet
        while (cursor + USB_PACKET_SIZE <= rx_ptr)
        {
            // Hunt for Start-of-Frame (0xFA)
            if (rx_buffer[cursor] != 0xFA)
            {
                cursor++;
                continue;
            }

            uint8_t receivedCrc = rx_buffer[cursor + 1];
            uint8_t dataLen = rx_buffer[cursor + 2];

            // Validate structure (Length sanity and Footer 0xBF)
            if (dataLen <= MAX_USB_DATA && rx_buffer[cursor + 3 + dataLen] == 0xBF)
            {
                // Verify checksum (CRC usually covers Length + Data)
                if (CRC8(rx_buffer + cursor + 2, dataLen + 1) == receivedCrc)
                {
                    // --- VALID PACKET FOUND ---
                    // Append directly to the passed reference
                    OutBuffer.Append((const char *)(rx_buffer + cursor + 3), dataLen);

                    // Consume the fixed-size frame
                    uint32_t total_consumed = cursor + USB_PACKET_SIZE;
                    rx_ptr -= total_consumed;

                    if (rx_ptr > 0)
                    {
                        memmove(rx_buffer, rx_buffer + total_consumed, rx_ptr);
                    }

                    return (uint16_t)dataLen;
                }
            }

            // False positive: skip header and keep hunting
            cursor++;
        }

        // 3. Maintenance: Clear scanned junk
        if (cursor > 0)
        {
            rx_ptr -= cursor;
            if (rx_ptr > 0)
            {
                memmove(rx_buffer, rx_buffer + cursor, rx_ptr);
            }
        }

        return 0;
    }

#elif defined BOARD_Valu_v2_0
    extern "C"
    {
#include "tusb.h"
        void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
        void USB_LP_CAN1_RX0_IRQHandler(void) { tud_int_handler(0); }
        // Called when device is mounted
        void tud_mount_cb(void) {}
        // Called when device is unmounted
        void tud_umount_cb(void) {}
        // Called when usb bus is suspended
        void tud_suspend_cb(bool remote_wakeup_en) {}
        // Called when usb bus is resumed
        void tud_resume_cb(void) {}
        void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
        {
            if (dtr)
            {
                // Terminal connected
            }
        }

        // Called when CDC receives new data
        void tud_cdc_rx_cb(uint8_t itf)
        {
            // Optional: wake up a task to read data
        }
    }

    void USB_Init()
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3);
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

        RCC_APB1PeriphResetCmd(RCC_APB1Periph_USB, ENABLE);
        HW::Sleep(1); // Reset timer
        RCC_APB1PeriphResetCmd(RCC_APB1Periph_USB, DISABLE);

        tusb_init();

        NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1);
        NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

        (EXTEN->EXTEN_CTR) |= EXTEN_USBD_PU_EN; // Pullup D+
    }

    void USB_Send(const FlexArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr)
            return;

        uint32_t offset = 0;
        // TinyUSB usually benefits from 4-byte alignment for DMA
        static uint8_t __attribute__((aligned(4))) packet[USB_PACKET_SIZE];

        while (offset < Data.Length)
        {
            memset(packet, 0, USB_PACKET_SIZE);

            // Calculate payload size
            uint8_t to_copy = (uint8_t)((Data.Length - offset > MAX_USB_DATA) ? MAX_USB_DATA : Data.Length - offset);

            // --- PACKET CONSTRUCTION ---
            packet[0] = 0xFA;    // Header
            packet[2] = to_copy; // Payload Length
            memcpy(packet + 3, Data.Array + offset, to_copy);
            packet[3 + to_copy] = 0xBF; // Footer

            // CRC8 covers the length byte and the payload
            packet[1] = CRC8(packet + 2, to_copy + 1);

            // Wait for space in the TinyUSB TX buffer
            uint32_t timeout = 1000; // Safety timeout
            while (tud_cdc_write_available() < USB_PACKET_SIZE && timeout > 0)
            {
                tud_task(); // Keep USB stack alive
                timeout--;
            }

            if (timeout == 0)
                break; // Failed to send

            tud_cdc_write(packet, USB_PACKET_SIZE);
            tud_cdc_write_flush();

            offset += to_copy;
            tud_task();
        }
    }

    uint16_t USB_Read(FlexArray &OutBuffer)
{
    static uint8_t rx_buffer[512];
    static uint32_t rx_ptr = 0;

    // 1. Pull new data from TinyUSB into the static buffer
    uint32_t available = tud_cdc_available();
    if (available > 0)
    {
        uint32_t space = sizeof(rx_buffer) - rx_ptr;
        uint32_t to_read = (available > space) ? space : available;

        if (to_read > 0)
        {
            // Direct read into our processing buffer
            tud_cdc_read(rx_buffer + rx_ptr, to_read);
            rx_ptr += to_read;
        }
    }

    uint32_t cursor = 0;

    // 2. Scan through the buffer for a full USB packet
    while (cursor + USB_PACKET_SIZE <= rx_ptr)
    {
        // Hunt for Start-of-Frame (0xFA)
        if (rx_buffer[cursor] != 0xFA)
        {
            cursor++;
            continue;
        }

        uint8_t receivedCrc = rx_buffer[cursor + 1];
        uint8_t dataLen     = rx_buffer[cursor + 2];

        // Validate structure (Length sanity and Footer 0xBF)
        // Check footer relative to the frame start (cursor + 3 + dataLen)
        if (dataLen <= MAX_USB_DATA && rx_buffer[cursor + 3 + dataLen] == 0xBF)
        {
            // Verify checksum
            if (CRC8(rx_buffer + cursor + 2, dataLen + 1) == receivedCrc)
            {
                // --- VALID PACKET FOUND ---
                // Append payload directly to the persistent buffer
                OutBuffer.Append((const char *)(rx_buffer + cursor + 3), dataLen);

                // Consume the full frame (usually 64 bytes)
                uint32_t total_consumed = cursor + USB_PACKET_SIZE;
                rx_ptr -= total_consumed;

                if (rx_ptr > 0)
                {
                    memmove(rx_buffer, rx_buffer + total_consumed, rx_ptr);
                }

                return (uint16_t)dataLen;
            }
        }

        // False positive: skip header and keep hunting
        cursor++;
    }

    // 3. Maintenance: Clear scanned junk
    if (cursor > 0)
    {
        rx_ptr -= cursor;
        if (rx_ptr > 0)
        {
            memmove(rx_buffer, rx_buffer + cursor, rx_ptr);
        }
    }

    return 0;
}

#endif
}