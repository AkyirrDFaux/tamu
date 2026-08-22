#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "Core/Functions/Packet.h"
#include "Core/Services/LogHandler.h"

// Reports an accelerometer/gyroscope error to the log service.
static inline void ReportAccGyrError(AccGyrError code) {
    ReportLog(MakeLog(true, (uint16_t)BlockType::AccGyr, (uint16_t)code, 0));
}

static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t lsm6ds3_handle;

// Reads `size` bytes starting at register `reg`. Combined transmit(receive with a finite
// timeout, matching the previously-working I2C driver. Internal pullups + RC_FAST clock are
// required: without pullups the bus floats low and every read returns ESP_OK with zeros.
static esp_err_t LSM6DS3ReadRegs(uint8_t reg, uint8_t *data, uint8_t size)
{
    return i2c_master_transmit_receive(lsm6ds3_handle, &reg, 1, data, size, 1000);
}

// Reads the WHO_AM_I register (0x0F); returns true when the LSM6DS3 answers and the value
// matches the LSM6DS3 family (0x69, some variants 0x6A).
static bool LSM6DS3ReadWho(uint8_t *who_out, esp_err_t *err_out)
{
    uint8_t who = 0;
    esp_err_t err = LSM6DS3ReadRegs(0x0F, &who, 1);
    if (err_out) *err_out = err;
    if (err != ESP_OK)
        return false;
    if (who_out) *who_out = who;
    return (who == 0x69 || who == 0x6A);
}

// Forces a restart of the LSM6DS3 over I2C (BOOT + SW_RESET in CTRL3_C, 0x12). BOOT
// re-initialises the part the same way a power cycle does, clearing the stuck state the
// sensor can occasionally get into after a cold boot (this board needs it sometimes).
static void LSM6DS3ForceRestart()
{
    uint8_t boot_cmd[] = {0x12, 0b10000001};
    i2c_master_transmit(lsm6ds3_handle, boot_cmd, 2, 1000);
    vTaskDelay(pdMS_TO_TICKS(30));
}

// Configures the accel (CTRL1_XL) and gyro (CTRL2_G) ODR/full-scale registers and verifies
// they read back the values just written. IF_INC (CTRL3_C bit 2) defaults to 1 after reset on
// this part, so the multi-byte config write steps 0x10 -> 0x11 and the 12-byte sensor read in
// ReadIMUData steps 0x22..0x2D. The IF_INC write is kept explicit for robustness.
static bool LSM6DS3Configure()
{
    // Enable address auto-increment (IF_INC, CTRL3_C bit 2).
    uint8_t ifinc_cmd[] = {0x12, 0b00000100};
    if (i2c_master_transmit(lsm6ds3_handle, ifinc_cmd, 2, 1000) != ESP_OK)
        return false;

    // Consecutive Write: 0x10 (CTRL1_XL, accel 104 Hz +/-2 g) and 0x11 (CTRL2_G, gyro 104 Hz +/-2000 dps)
    uint8_t config_cmd[] = {0x10, 0b01000100, 0b01001100};
    if (i2c_master_transmit(lsm6ds3_handle, config_cmd, 3, 1000) != ESP_OK)
        return false;

    // Verify the ODR registers actually stuck (writes into a stuck/mid-boot part are dropped).
    uint8_t ctrl1 = 0, ctrl2 = 0;
    if (LSM6DS3ReadRegs(0x10, &ctrl1, 1) != ESP_OK) return false;
    if (LSM6DS3ReadRegs(0x11, &ctrl2, 1) != ESP_OK) return false;
    return (ctrl1 == 0b01000100 && ctrl2 == 0b01001100);
}

// Initialises the I2C master bus and configures the LSM6DS3 IMU. Mirrors the previously
// working driver (internal pullups, RC_FAST clock, pin reset, finite timeouts) and adds a
// forced I2C restart + retry, because the part occasionally needs one before it accepts
// configuration.
void InitLSM6DS3() {
    // 1. Reset the GPIOs to their default state before the I2C driver takes them over.
    gpio_reset_pin((gpio_num_t)GPIO_NUM_4);
    gpio_reset_pin((gpio_num_t)GPIO_NUM_5);

    // 1b. Diagnostic: with internal pullups both lines must read high when nothing drives
    //     them. A stuck-low line means a hardware fault (short / dead IMU holding SDA) and
    //     explains "ESP_OK but all-zero reads" regardless of the driver settings.
    gpio_set_direction((gpio_num_t)GPIO_NUM_4, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)GPIO_NUM_5, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)GPIO_NUM_4, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)GPIO_NUM_5, GPIO_PULLUP_ONLY);
    ESP_LOGW("LSM6DS3", "Bus lines with pullup: SDA(GPIO4)=%d SCL(GPIO5)=%d",
             gpio_get_level((gpio_num_t)GPIO_NUM_4), gpio_get_level((gpio_num_t)GPIO_NUM_5));

    // 2. Initialize I2C Master Bus. Internal pullups are essential: this board has no
    //    external pull-ups on SDA/SCL, and without them the bus floats low so every
    //    transaction "succeeds" (ESP_OK) but reads return all zeros.
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_5,
        .clk_source = I2C_CLK_SRC_RC_FAST,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&bus_cfg, &i2c_bus_handle) != ESP_OK) {
        ReportAccGyrError(ErrBusGeneric);
        return;
    }

    // 3. Add LSM6DS3TRC Device (address 0x6A)
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6A,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {},
    };
    if (i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &lsm6ds3_handle) != ESP_OK) {
        ReportAccGyrError(ErrDeviceNotFound);
        return;
    }

    bool configured = false;
    uint8_t who = 0;
    esp_err_t who_err = ESP_OK;

    // First try without forcing a restart (the sensor is usually healthy); force it only
    // when WHO_AM_I is absent, then re-verify the configuration afterwards.
    for (int attempt = 0; attempt < 5 && !configured; attempt++)
    {
        if (attempt > 0)
            LSM6DS3ForceRestart();

        if (!LSM6DS3ReadWho(&who, &who_err))
            continue; // unresponsive / wrong WHO_AM_I; force another restart

        configured = LSM6DS3Configure();
    }

    if (configured)
    {
        ESP_LOGI("LSM6DS3", "Configured (WHO_AM_I=0x%02X).", who);
    }
    else
    {
        ESP_LOGE("LSM6DS3", "Init failed after retries: WHO_AM_I=0x%02X err=0x%x", who, who_err);
        ReportAccGyrError(ErrInitFailed);
    }
}

// Reads raw gyro/accel registers and updates the smoothed (low-pass filtered) values in the block.
bool ReadIMUData() {
    uint16_t Raw[6];
    
    // Read 12 bytes starting from register 0x22 (gyro X/Y/Z then accel X/Y/Z)
    if (LSM6DS3ReadRegs(0x22, (uint8_t *)Raw, 12) != ESP_OK) {
        ReportAccGyrError(ErrBusGeneric);
        return false;
    }

    // Data Processing (scale factors /209 and /939 match the previously-working driver)
    Number AccInvW = 1 / (1 + AccGyr.AccFilter);
    Number AccW = 1 - AccInvW;
    Number RotInvW = 1 / (1 + AccGyr.GyroFilter);
    Number RotW = 1 - RotInvW;

    AccGyr.AngularVelocity.Data[0] = (Number(Raw[0]) / N(939.0)) * RotInvW + (AccGyr.AngularVelocity.Data[0] * RotW);
    AccGyr.AngularVelocity.Data[1] = (Number(Raw[1]) / N(939.0)) * RotInvW + (AccGyr.AngularVelocity.Data[1] * RotW);
    AccGyr.AngularVelocity.Data[2] = (Number(Raw[2]) / N(939.0)) * RotInvW + (AccGyr.AngularVelocity.Data[2] * RotW);

    AccGyr.Acceleration.Data[0] = (Number(Raw[3]) / N(209.0)) * AccInvW + (AccGyr.Acceleration.Data[0] * AccW);
    AccGyr.Acceleration.Data[1] = (Number(Raw[4]) / N(209.0)) * AccInvW + (AccGyr.Acceleration.Data[1] * AccW);
    AccGyr.Acceleration.Data[2] = (Number(Raw[5]) / N(209.0)) * AccInvW + (AccGyr.Acceleration.Data[2] * AccW);

    return true;
}

// Placeholder callback for accelerometer frequency changes; currently accepts all changes.
bool OnAccGyrFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    return true;
}