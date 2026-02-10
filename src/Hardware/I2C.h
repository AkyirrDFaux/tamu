#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "driver/i2c_master.h"
typedef i2c_master_bus_handle_t I2C_Handle;
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

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
bool I2C::Begin(const Pin &SCL, const Pin &SDA, uint32_t Speed)
{
    gpio_reset_pin((gpio_num_t)SCL.Number);
    gpio_reset_pin((gpio_num_t)SDA.Number);
    gpio_set_direction((gpio_num_t)SDA.Number, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)SCL.Number, GPIO_MODE_INPUT_OUTPUT_OD);
    
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0, // C3 has one I2C peripheral (I2C_NUM_0)
        .sda_io_num = (gpio_num_t)SDA.Number,
        .scl_io_num = (gpio_num_t)SCL.Number,
        .clk_source = I2C_CLK_SRC_RC_FAST,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0, // Blocking mode
        .flags = {.enable_internal_pullup = false}};

    // Initialize the bus and store the handle
    esp_err_t err = i2c_new_master_bus(&bus_config, &Handle);
    return (err == ESP_OK);
}

bool I2C::Write(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle) return false;

    // 1. We have to "add" the device to the bus temporarily to get a dev_handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = Address,
        .scl_speed_hz = 100000, // Or pass your 'Speed' from Begin()
    };

    i2c_master_dev_handle_t dev_handle;
    if (i2c_master_bus_add_device(Handle, &dev_cfg, &dev_handle) != ESP_OK) {
        return false;
    }

    // 2. Prepare the buffer (Register + Data)
    size_t total_len = Length + 1;
    uint8_t *buffer = (uint8_t *)malloc(total_len);
    if (!buffer) {
        i2c_master_bus_rm_device(dev_handle);
        return false;
    }

    buffer[0] = Register;
    if (Data && Length > 0) {
        memcpy(&buffer[1], Data, Length);
    }

    // 3. Transmit using the dev_handle
    esp_err_t err = i2c_master_transmit(dev_handle, buffer, total_len, -1);
    
    // 4. Cleanup
    free(buffer);
    i2c_master_bus_rm_device(dev_handle); // Important! Don't leak device handles
    
    return (err == ESP_OK);
}

bool I2C::Read(uint8_t Address, uint8_t Register, uint8_t *Data, uint16_t Length)
{
    if (!Handle || !Data) return false;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = Address,
        .scl_speed_hz = 400000,
    };

    i2c_master_dev_handle_t dev_handle;
    if (i2c_master_bus_add_device(Handle, &dev_cfg, &dev_handle) != ESP_OK) {
        return false;
    }

    // Transmit register address, then receive data
    esp_err_t err = i2c_master_transmit_receive(dev_handle, &Register, 1, Data, Length, -1);

    i2c_master_bus_rm_device(dev_handle);
    return (err == ESP_OK);
}
#elif defined BOARD_Valu_v2_0

#define I2C_EVENT_SB ((uint16_t)0x0001)   // Start Bit (Master)
#define I2C_EVENT_ADDR ((uint16_t)0x0002) // Address Sent
#define I2C_EVENT_TXE ((uint16_t)0x0080)  // Transmit Empty
#define I2C_EVENT_RXNE ((uint16_t)0x0040) // Receive Not Empty
#define I2C_EVENT_BTF ((uint16_t)0x0004)  // Byte Transfer Finished
#define I2C_BUSY ((uint16_t)0x0002)       // Bus Busy (STAR2)

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