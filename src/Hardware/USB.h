namespace HW
{
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
            .tx_buffer_size = 512,
            .rx_buffer_size = 512,
        };
        usb_serial_jtag_driver_install(&usb_config);
    }

    void USB_Send(const ByteArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr)
            return;

        const uint32_t MAX_DATA = 60;
        uint32_t offset = 0;

        while (offset < Data.Length)
        {
            uint8_t packet[64] = {0}; // Local buffer is fine on ESP32 stack
            uint8_t to_copy = (uint8_t)((Data.Length - offset > MAX_DATA) ? MAX_DATA : Data.Length - offset);

            packet[0] = 0xFA;
            packet[2] = to_copy;
            memcpy(packet + 3, Data.Array + offset, to_copy);
            packet[3 + to_copy] = 0xBF;

            // CRC on [Len + Data]
            packet[1] = CRC8(packet + 2, to_copy + 1);

            // In ESP-IDF, this function blocks or writes to an internal ring buffer
            usb_serial_jtag_write_bytes(packet, 64, pdMS_TO_TICKS(10));

            offset += to_copy;
        }
    }

    ByteArray USB_Read()
    {
        static uint8_t rx_buffer[256]; // Increased for headroom
        static uint32_t rx_ptr = 0;

        // 1. Read available bytes into buffer
        // Non-blocking read (ticks_to_wait = 0)
        int len = usb_serial_jtag_read_bytes(rx_buffer + rx_ptr, (sizeof(rx_buffer) - rx_ptr), 0);
        if (len > 0)
        {
            rx_ptr += len;
        }

        // 2. Process for 64-byte frame
        while (rx_ptr >= 64)
        {
            if (rx_buffer[0] == 0xFA)
            {
                uint8_t receivedCrc = rx_buffer[1];
                uint8_t dataLen = rx_buffer[2];

                if (dataLen <= 60)
                {
                    if (CRC8(rx_buffer + 2, dataLen + 1) == receivedCrc)
                    {
                        if (rx_buffer[3 + dataLen] == 0xBF)
                        {
                            // SUCCESS
                            ByteArray Data;
                            Data.Length = dataLen;
                            Data.Array = new char[dataLen];
                            memcpy(Data.Array, rx_buffer + 3, dataLen);

                            // Shift buffer
                            memmove(rx_buffer, rx_buffer + 64, rx_ptr - 64);
                            rx_ptr -= 64;
                            return Data;
                        }
                    }
                }
                // Invalid packet logic: slide 1
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
            else
            {
                // No header: slide 1
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
        }
        return ByteArray();
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

    void USB_Send(const ByteArray &Data)
    {
        if (Data.Length == 0 || Data.Array == nullptr)
            return;

        const uint32_t MAX_DATA = 60;
        uint32_t offset = 0;

        while (offset < Data.Length)
        {
            static uint8_t __attribute__((aligned(4))) packet[64];
            memset(packet, 0, 64);

            uint8_t to_copy = (uint8_t)((Data.Length - offset > MAX_DATA) ? MAX_DATA : Data.Length - offset);

            packet[0] = 0xFA;
            packet[2] = to_copy; // Length byte
            memcpy(packet + 3, Data.Array + offset, to_copy);
            packet[3 + to_copy] = 0xBF;

            // Calculate CRC on [Len + Data]
            // This includes packet[2] through the end of the data payload
            packet[1] = CRC8(packet + 2, to_copy + 1);

            uint32_t timeout = 10000;
            while (tud_cdc_write_available() < 64 && timeout > 0)
            {
                tud_task();
                timeout--;
            }

            tud_cdc_write(packet, 64);
            tud_cdc_write_flush();
            offset += to_copy;
            tud_task();
        }
    }
    // Static buffer to survive between USB_Read() calls
    static uint8_t rx_buffer[128];
    static uint32_t rx_ptr = 0;

    ByteArray USB_Read()
    {
        // 1. Pull everything available from TinyUSB into our local ring buffer
        uint32_t count = tud_cdc_available();
        if (count > 0)
        {
            // Prevent overflow
            if (rx_ptr + count > 128)
                rx_ptr = 0;
            tud_cdc_read(rx_buffer + rx_ptr, count);
            rx_ptr += count;
        }

        // 2. Process our local buffer for a 64-byte frame
        while (rx_ptr >= 64)
        {
            if (rx_buffer[0] == 0xFA)
            {
                uint8_t receivedCrc = rx_buffer[1];
                uint8_t dataLen = rx_buffer[2];

                if (dataLen <= 60)
                {
                    // Check CRC
                    if (CRC8(rx_buffer + 2, dataLen + 1) == receivedCrc)
                    {
                        // Check Footer
                        if (rx_buffer[3 + dataLen] == 0xBF)
                        {

                            // SUCCESS: Construct return object
                            ByteArray Data;
                            Data.Length = dataLen;
                            Data.Array = new char[dataLen];
                            memcpy(Data.Array, rx_buffer + 3, dataLen);

                            // Shift buffer by 64
                            memmove(rx_buffer, rx_buffer + 64, rx_ptr - 64);
                            rx_ptr -= 64;
                            return Data;
                        }
                    }
                }
                // If header found but check failed, slide 1 to find next 0xFA
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
            else
            {
                // No header at 0, slide 1
                memmove(rx_buffer, rx_buffer + 1, rx_ptr - 1);
                rx_ptr--;
            }
        }

        return ByteArray(); // Nothing valid found yet
    }

#endif
}