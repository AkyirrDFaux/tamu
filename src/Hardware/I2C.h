#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "driver/i2c_master.h"
typedef i2c_master_bus_handle_t I2C_Handle;
struct I2C_Device
{
    uint8_t Address;
    i2c_master_dev_handle_t DevHandle;
};
#elif defined BOARD_Valu_v2_0
typedef I2C_TypeDef *I2C_Handle;
#endif

class I2C
{
public:
    I2C_Handle Handle;
    uint32_t BusSpeed;

    I2C();
    ~I2C();

    bool Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed);
    bool Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length);
    bool Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length);

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
    I2C_Device Devices[8];
    uint8_t DeviceCount;

private:
    i2c_master_dev_handle_t GetOrCreateDevice(uint8_t Address);
#else
    void BusScan();
#endif
};

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0

I2C::I2C() : Handle(nullptr), DeviceCount(0)
{
    for (int i = 0; i < 8; i++)
        Devices[i] = {0, nullptr};
}

I2C::~I2C()
{
    if (Handle)
    {
        // Remove all cached devices first
        for (uint8_t i = 0; i < DeviceCount; i++)
        {
            if (Devices[i].DevHandle)
            {
                i2c_master_bus_rm_device(Devices[i].DevHandle);
                Devices[i].DevHandle = nullptr;
            }
        }

        // Now it's safe to delete the bus
        i2c_del_master_bus(Handle);
        Handle = nullptr;
    }
}

bool I2C::Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed)
{
    BusSpeed = Speed;
    gpio_reset_pin((gpio_num_t)SCL.Number);
    gpio_reset_pin((gpio_num_t)SDA.Number);

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = (gpio_num_t)SDA.Number;
    bus_config.scl_io_num = (gpio_num_t)SCL.Number;
    bus_config.clk_source = I2C_CLK_SRC_RC_FAST;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    return (i2c_new_master_bus(&bus_config, &Handle) == ESP_OK);
}

i2c_master_dev_handle_t I2C::GetOrCreateDevice(uint8_t Address)
{
    for (uint8_t i = 0; i < DeviceCount; i++)
    {
        if (Devices[i].Address == Address)
            return Devices[i].DevHandle;
    }

    if (DeviceCount < 8)
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = Address,
            .scl_speed_hz = BusSpeed,
            .scl_wait_us = 0};

        if (i2c_master_bus_add_device(Handle, &dev_cfg, &Devices[DeviceCount].DevHandle) == ESP_OK)
        {
            Devices[DeviceCount].Address = Address;
            return Devices[DeviceCount++].DevHandle;
        }
    }
    return nullptr;
}

bool I2C::Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle)
        return false;

    // Stack buffer within the write function as requested
    uint8_t LocalWriteBuffer[256];
    if (Length >= sizeof(LocalWriteBuffer))
        return false;

    i2c_master_dev_handle_t dev_handle = GetOrCreateDevice(Address);
    if (!dev_handle)
        return false;

    LocalWriteBuffer[0] = Register;
    if (Data && Length > 0)
    {
        memcpy(&LocalWriteBuffer[1], Data, Length);
    }

    return (i2c_master_transmit(dev_handle, LocalWriteBuffer, Length + 1, 1000) == ESP_OK);
}

bool I2C::Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle || !Data)
        return false;

    i2c_master_dev_handle_t dev_handle = GetOrCreateDevice(Address);
    if (!dev_handle)
        return false;

    return (i2c_master_transmit_receive(dev_handle, &Register, 1, Data, Length, 1000) == ESP_OK);
}

#elif defined BOARD_Valu_v2_0

#define I2C_EVENT_SB ((uint16_t)0x0001)   // Start Bit (Master)
#define I2C_EVENT_ADDR ((uint16_t)0x0002) // Address Sent
#define I2C_EVENT_TXE ((uint16_t)0x0080)  // Transmit Empty
#define I2C_EVENT_RXNE ((uint16_t)0x0040) // Receive Not Empty
#define I2C_EVENT_BTF ((uint16_t)0x0004)  // Byte Transfer Finished
#define I2C_BUSY ((uint16_t)0x0002)       // Bus Busy (STAR2)

I2C::I2C() {};
I2C::~I2C() {};

bool I2C::Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed)
{
    // 1. Auto-Detection Logic
    if (SCL.Port == 'B' && SCL.Number == 6 && SDA.Port == 'B' && SDA.Number == 7)
    {
        Handle = I2C1;
        RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
        RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
        RCC->APB2PCENR |= RCC_APB2Periph_AFIO; // Add this!
        // PB6, PB7: AF Open-Drain (using GetPort to resolve 'B')
        GPIO_TypeDef *port = GetPort(SCL.Port);
        // For PB6 and PB7, use CFGLR
        port->CFGLR &= ~(0xFF << 24); // Pin 6 starts at bit 24, Pin 7 at bit 28
        port->CFGLR |= (0xEE << 24);
    }
    else if (SCL.Port == 'B' && SCL.Number == 10 && SDA.Port == 'B' && SDA.Number == 11)
    {
        Handle = I2C2;
        RCC->APB1PCENR |= RCC_APB1Periph_I2C2;
        RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
        RCC->APB2PCENR |= RCC_APB2Periph_AFIO; // Add this!
        // PB10, PB11: AF Open-Drain
        GPIO_TypeDef *port = GetPort(SCL.Port);
        port->CFGHR &= ~(0xFF << 8);
        port->CFGHR |= (0xEE << 8);
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
    if (Speed <= 100000)
    {
        Handle->CKCFGR = 36000000 / (Speed * 2);
    }
    else
    {
        // Fast Mode (400kHz+) usually requires different Duty cycle bits
        Handle->CKCFGR = (36000000 / (Speed * 3)) | I2C_CKCFGR_FS;
    }

    Handle->CTLR1 |= I2C_CTLR1_PE;

    return true;
}

bool I2C::Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle)
        return false;

    uint32_t timeout;
    auto WaitWithTimeout = [&](uint16_t event) -> bool
    {
        timeout = 20000; // Adjust based on clock
        while (!(Handle->STAR1 & event))
        {
            // Check for Acknowledge Failure (NACK)
            if (Handle->STAR1 & (1 << 10))
            { // AF bit is bit 10
                Handle->CTLR1 |= I2C_CTLR1_STOP;
                Handle->STAR1 &= ~(1 << 10); // Clear AF
                return false;
            }
            if (--timeout == 0)
                return false;
        }
        return true;
    };

    // 1. Bus Busy
    timeout = 10000;
    while ((Handle->STAR2 & I2C_BUSY) && --timeout)
        ;
    if (timeout == 0)
        return false;

    // 2. Start
    Handle->CTLR1 |= I2C_CTLR1_START;
    if (!WaitWithTimeout(I2C_EVENT_SB))
        return false;

    // 3. Address
    Handle->DATAR = (Address << 1);
    if (!WaitWithTimeout(I2C_EVENT_ADDR))
        return false;
    (void)Handle->STAR2;

    // 4. Register
    Handle->DATAR = Register;
    if (!WaitWithTimeout(I2C_EVENT_TXE))
        return false;

    // 5. Data Burst
    for (uint16_t i = 0; i < Length; i++)
    {
        Handle->DATAR = Data[i];
        if (!WaitWithTimeout(I2C_EVENT_TXE))
            return false;
    }

    // 6. Stop
    timeout = 10000;
    while (!(Handle->STAR1 & I2C_EVENT_BTF) && --timeout)
        ;
    Handle->CTLR1 |= I2C_CTLR1_STOP;

    return true;
}

bool I2C::Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle || Data == nullptr)
        return false;

    uint32_t timeout = 20000;

    // --- 1. START ---
    Handle->CTLR1 |= I2C_CTLR1_START;
    while (!(Handle->STAR1 & I2C_EVENT_SB) && --timeout)
        ;
    if (!timeout)
        return false;

    // --- 2. DEVICE ADDRESS (WRITE) ---
    Handle->DATAR = (Address << 1);
    timeout = 20000;
    while (!(Handle->STAR1 & I2C_EVENT_ADDR) && --timeout)
    {
        if (Handle->STAR1 & (1 << 10))
        { // AF: NACK
            Handle->CTLR1 |= I2C_CTLR1_STOP;
            Handle->STAR1 &= ~(1 << 10);
            return false;
        }
    }
    if (!timeout)
        return false;

    (void)Handle->STAR2;

    // --- 3. REGISTER ADDRESS ---
    Handle->DATAR = Register;
    timeout = 20000;
    // Using BTF here ensures the byte is physically shifted out before Re-Start
    while (!(Handle->STAR1 & (1 << 2)) && --timeout)
        ; // 1<<2 is BTF
    if (!timeout)
        return false;

    // --- 4. REPEATED START ---
    Handle->CTLR1 |= I2C_CTLR1_START;
    timeout = 20000;
    while (!(Handle->STAR1 & I2C_EVENT_SB) && --timeout)
        ;
    if (!timeout)
        return false;

    // --- 5. DEVICE ADDRESS (READ) ---
    Handle->DATAR = (Address << 1) | 1;
    timeout = 20000;
    while (!(Handle->STAR1 & I2C_EVENT_ADDR) && --timeout)
    {
        if (Handle->STAR1 & (1 << 10))
        {
            Handle->CTLR1 |= I2C_CTLR1_STOP;
            Handle->STAR1 &= ~(1 << 10);
            return false;
        }
    }
    if (!timeout)
    {
        return false;
    }

    // --- 6. RECEIVE LOGIC ---
    if (Length == 1)
    {
        Handle->CTLR1 &= ~I2C_CTLR1_ACK;
        (void)Handle->STAR2;
        Handle->CTLR1 |= I2C_CTLR1_STOP;

        timeout = 20000;
        while (!(Handle->STAR1 & I2C_EVENT_RXNE) && --timeout)
            ;
        if (!timeout)
            return false;
        Data[0] = Handle->DATAR;
    }
    else
    {
        (void)Handle->STAR2;
        for (uint16_t i = 0; i < Length; i++)
        {
            if (i == Length - 1)
            {
                Handle->CTLR1 &= ~I2C_CTLR1_ACK;
                Handle->CTLR1 |= I2C_CTLR1_STOP;
            }
            timeout = 20000;
            while (!(Handle->STAR1 & I2C_EVENT_RXNE) && --timeout)
                ;
            if (!timeout)
                return false;
            Data[i] = Handle->DATAR;
        }
    }

    Handle->CTLR1 |= I2C_CTLR1_ACK;
    return true;
}

void I2C::BusScan()
{
    // Start-up notification
    HW::NotificationBlink(1, 10);

    for (uint8_t i = 1; i < 127; i++)
    {
        uint32_t timeout = 5000; // Safety timeout for 144MHz cycles

        // 1. Generate START
        Handle->CTLR1 |= I2C_CTLR1_START;
        while (!(Handle->STAR1 & I2C_EVENT_SB) && --timeout)
            ;

        if (timeout == 0)
        {
            // Bus is likely locked up, attempt a peripheral reset
            Handle->CTLR1 |= I2C_CTLR1_SWRST;
            for (volatile int d = 0; d < 100; d++)
                ;
            Handle->CTLR1 &= ~I2C_CTLR1_SWRST;
            continue;
        }

        // 2. Send Address (Write bit 0)
        Handle->DATAR = (i << 1);

        // 3. Wait for ADDR (Success) or AF (Acknowledge Failure)
        timeout = 5000;
        while (!(Handle->STAR1 & (I2C_EVENT_ADDR | (1 << 10))) && --timeout)
            ;

        if (Handle->STAR1 & I2C_EVENT_ADDR)
        {
            // --- SUCCESS: Device Found ---
            Handle->CTLR1 |= I2C_CTLR1_STOP;
            (void)Handle->STAR2; // Clear ADDR flag

            // Prepare a report message
            char msg[16];
            // Simple hex conversion for "Found: 0xXX"
            msg[0] = 'I';
            msg[1] = '2';
            msg[2] = 'C';
            msg[3] = ':';
            msg[4] = ' ';
            msg[5] = '0';
            msg[6] = 'x';
            msg[7] = "0123456789ABCDEF"[(i >> 4) & 0x0F];
            msg[8] = "0123456789ABCDEF"[i & 0x0F];

            // Use your new Report function (assuming Status::OK or similar)
            ReportError(Status::OK, 9, msg);

            HW::NotificationBlink(2, 50);
        }
        else
        {
            // --- FAILURE: No ACK ---
            Handle->CTLR1 |= I2C_CTLR1_STOP;
            Handle->STAR1 &= ~(1 << 10); // Clear AF (NACK) flag

            // Short delay to let the lines settle at 144MHz
            for (volatile int d = 0; d < 2000; d++)
                ;
        }

        // Mandatory yield to USB/System between pings
        HW::tud_task();
    }
}
#endif