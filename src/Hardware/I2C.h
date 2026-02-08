#if defined BOARD_Tamu_v1_0
#elif defined BOARD_Tamu_v2_0
#elif defined BOARD_Valu_v2_0
typedef I2C_TypeDef *I2C_Handle;
#endif

class I2C
{
private:
    I2C_Handle Handle;

public:
    I2C() : Handle(nullptr) {}

    // Auto-detects the hardware instance based on the pins provided
    bool Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed);

    bool Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length);
    bool Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length);
};

#if defined BOARD_Tamu_v1_0
#elif defined BOARD_Tamu_v2_0
#elif defined BOARD_Valu_v2_0

#define I2C_EVENT_SB    ((uint16_t)0x0001)  // Start Bit (Master)
#define I2C_EVENT_ADDR  ((uint16_t)0x0002)  // Address Sent
#define I2C_EVENT_TXE   ((uint16_t)0x0080)  // Transmit Empty
#define I2C_EVENT_RXNE  ((uint16_t)0x0040)  // Receive Not Empty
#define I2C_EVENT_BTF   ((uint16_t)0x0004)  // Byte Transfer Finished
#define I2C_BUSY        ((uint16_t)0x0002)  // Bus Busy (STAR2)

bool I2C::Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed)
{
    // 1. Auto-Detection Logic
    if (SCL.Port == GPIOB && SCL.Number == 6 && SDA.Port == GPIOB && SDA.Number == 7)
    {
        Handle = I2C1;
        RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
        RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;

        // PB6, PB7: AF Open-Drain
        SCL.Port->CFGHR &= ~(0xFF << 24);
        SCL.Port->CFGHR |= (0xEE << 24);
    }
    else if (SCL.Port == GPIOB && SCL.Number == 10 && SDA.Port == GPIOB && SDA.Number == 11)
    {
        Handle = I2C2;
        RCC->APB1PCENR |= RCC_APB1Periph_I2C2;
        RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;

        // PB10, PB11: AF Open-Drain
        SCL.Port->CFGHR &= ~(0xFF << 8);
        SCL.Port->CFGHR |= (0xEE << 8);
    }
    else
    {
        // These pins don't match any hardware I2C peripheral!
        Handle = nullptr;
        return false;
    }

    // 2. Hardware Setup (only runs if a valid handle was found)
    Handle->CTLR1 &= ~I2C_CTLR1_PE;
    Handle->CTLR2 = 36; // PCLK1 @ 36MHz
    Handle->CKCFGR = 36000000 / (Speed * 2);
    Handle->CTLR1 |= I2C_CTLR1_PE;

    return true;
}

bool I2C::Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle)
        return false;

    // 1. Wait until bus is not busy
    uint32_t timeout = 10000;
    while ((Handle->STAR2 & I2C_BUSY) && --timeout)
        ;
    if (timeout == 0)
        return false;

    // 2. Generate START condition
    Handle->CTLR1 |= I2C_CTLR1_START;
    while (!(Handle->STAR1 & I2C_EVENT_SB))
        ;

    // 3. Send Slave Address + Write Bit (0)
    Handle->DATAR = (Address << 1);
    while (!(Handle->STAR1 & I2C_EVENT_ADDR))
        ;
    (void)Handle->STAR2; // Clear ADDR flag by reading STAR2

    // 4. Send Register Address
    Handle->DATAR = Register;
    while (!(Handle->STAR1 & I2C_EVENT_TXE))
        ;

    // 5. Send Data Burst
    if (Data != nullptr && Length > 0)
    {
        for (uint16_t i = 0; i < Length; i++)
        {
            Handle->DATAR = Data[i];
            while (!(Handle->STAR1 & I2C_EVENT_TXE))
                ;
        }
    }

    // 6. Finalize: Wait for last byte to finish and send STOP
    while (!(Handle->STAR1 & I2C_EVENT_BTF))
        ;
    Handle->CTLR1 |= I2C_CTLR1_STOP;

    return true;
}

bool I2C::Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle || Data == nullptr)
        return false;

    // 1. Send the register address first (Write mode)
    if (!Write(Address, Register, nullptr, 0))
        return false;

    // 2. Generate Re-START
    Handle->CTLR1 |= I2C_CTLR1_START;
    while (!(Handle->STAR1 & I2C_EVENT_SB))
        ;

    // 3. Send Slave Address + Read Bit (1)
    Handle->DATAR = (Address << 1) | 1;
    while (!(Handle->STAR1 & I2C_EVENT_ADDR))
        ;
    (void)Handle->STAR2; // Clear ADDR flag

    // 4. Receive Data
    for (uint16_t i = 0; i < Length; i++)
    {
        if (i == Length - 1)
        {
            Handle->CTLR1 &= ~I2C_CTLR1_ACK; // NACK the very last byte
            Handle->CTLR1 |= I2C_CTLR1_STOP; // Prepare STOP
        }

        while (!(Handle->STAR1 & I2C_EVENT_RXNE))
            ;
        Data[i] = Handle->DATAR;
    }

    // 5. Cleanup for next transaction
    Handle->CTLR1 |= I2C_CTLR1_ACK; // Re-enable ACK for future reads
    return true;
}
#endif