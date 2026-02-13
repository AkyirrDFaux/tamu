#if defined BOARD_Valu_v2_0
extern "C"
{
#include "u8g2.h"
    uint8_t u8x8_byte_ch32_hw_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
    {
        switch (msg)
        {
        case U8X8_MSG_BYTE_SEND:
        {
            uint8_t *data = (uint8_t *)arg_ptr;
            while (arg_int--)
            {
                while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
                    ;
                SPI_I2S_SendData(SPI1, *data++);
            }
            break;
        }
        case U8X8_MSG_BYTE_SET_DC:
            // CRITICAL: Wait until SPI is no longer busy before changing DC
            while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
                ;
            GPIO_WriteBit(GPIOB, GPIO_Pin_0, (arg_int ? Bit_SET : Bit_RESET));
            break;
        }
        return 1;
    }

    uint8_t u8x8_gpio_and_delay_ch32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
    {
        switch (msg)
        {
        case U8X8_MSG_DELAY_MILLI:
            for (int i = 0; i < arg_int; i++)
            {
                HW::Sleep(1);
                HW::tud_task(); // Keep USB alive during the OLED startup delay
            }
            break;
        case U8X8_MSG_GPIO_CS:
            // CS is on PA4
            GPIO_WriteBit(GPIOA, GPIO_Pin_4, (arg_int ? Bit_SET : Bit_RESET));
            break;
        case U8X8_MSG_GPIO_RESET:
            // Reset is on PA3
            GPIO_WriteBit(GPIOA, GPIO_Pin_3, (arg_int ? Bit_SET : Bit_RESET));
            break;
        }
        return 1;
    }
}


namespace HW
{
    void OLED_Init(u8g2_t *u8g2)
    {
        GPIO_InitTypeDef GPIO_InitStructure = {0};
        SPI_InitTypeDef SPI_InitStructure = {0};

        // Enable GPIOA, GPIOB, and SPI1 clocks
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

        // SPI Pins: PA5 (CLK), PA7 (MOSI)
        GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOA, &GPIO_InitStructure);

        // Control Pins on Port A: PA3 (Reset), PA4 (CS)
        GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_Init(GPIOA, &GPIO_InitStructure);

        // Control Pin on Port B: PB0 (DC)
        GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_Init(GPIOB, &GPIO_InitStructure);

        // SPI Configuration
        SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
        SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
        SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
        SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
        SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
        SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
        SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_32; // 144MHz / 32 = 4.5MHz
        SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
        SPI_Init(SPI1, &SPI_InitStructure);
        NVIC_SetPriority(SPI1_IRQn, 2);
        SPI_Cmd(SPI1, ENABLE);

        u8g2_Setup_sh1106_128x64_vcomh0_f(u8g2, U8G2_R1, u8x8_byte_ch32_hw_spi, u8x8_gpio_and_delay_ch32);
        u8g2_InitDisplay(u8g2);
        u8g2_SetPowerSave(u8g2, 0);
    }
}
#endif
