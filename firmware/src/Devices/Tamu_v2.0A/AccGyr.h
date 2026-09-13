#pragma once

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

// Writes a single register and verifies the value reads back (writes into a stuck or
// mid-boot part are dropped silently, so the read-back is the source of truth).
static bool LSM6DS3WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t cmd[] = {reg, value};
    if (i2c_master_transmit(lsm6ds3_handle, cmd, 2, 1000) != ESP_OK)
        return false;
    uint8_t check = 0;
    if (LSM6DS3ReadRegs(reg, &check, 1) != ESP_OK)
        return false;
    return check == value;
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

    // Consecutive Write: 0x10 (CTRL1_XL, accel 104 Hz +/-16 g) and 0x11 (CTRL2_G, gyro 104 Hz +/-2000 dps).
    // FS_XL = 01 -> +/-16 g and FS_G = 11 -> +/-2000 dps per the LSM6DS3TR datasheet
    // (Tables 51/54); the raw divisors in ReadIMUData are calibrated for these settings.
    uint8_t config_cmd[] = {0x10, 0b01000100, 0b01001100};
    if (i2c_master_transmit(lsm6ds3_handle, config_cmd, 3, 1000) != ESP_OK)
        return false;

    // Verify the ODR registers actually stuck (writes into a stuck/mid-boot part are dropped).
    uint8_t ctrl1 = 0, ctrl2 = 0;
    if (LSM6DS3ReadRegs(0x10, &ctrl1, 1) != ESP_OK) return false;
    if (LSM6DS3ReadRegs(0x11, &ctrl2, 1) != ESP_OK) return false;
    return (ctrl1 == 0b01000100 && ctrl2 == 0b01001100);
}

// LSM6DS3TR register encodings (datasheet Tables 51/52/54/55). The enum fields store the
// INDEX into these tables; the triggers snap an out-of-range write and apply the result.
static const uint8_t AccGyrOdrCode[8] = {0b0001, 0b0010, 0b0011, 0b0100, 0b0101, 0b0110, 0b0111, 0b1000};
static const uint8_t AccGyrAccFsCode[4] = {0b00, 0b10, 0b11, 0b01}; // +-2, +-4, +-8, +-16 g (CTRL1_XL bits 3:2)
static const uint8_t AccGyrAngFsNib[5] = {0b0010, 0b0000, 0b0100, 0b1000, 0b1100}; // +-125..+-2000 dps (CTRL2_G bits 3:0)

// Builds the CTRL1_XL / CTRL2_G bytes from the block's ODR + full-scale selections
// (LPF1_BW_SEL / BW0_XL / bit 0 are left cleared).
static uint8_t AccGyrCtrl1Byte()
{
    uint8_t odr = AccGyr.SamplingRate < 8 ? AccGyrOdrCode[AccGyr.SamplingRate] : 0;
    uint8_t fs = AccGyr.RangeAcc < 4 ? AccGyrAccFsCode[AccGyr.RangeAcc] : 0;
    return (uint8_t)((odr << 4) | (fs << 2));
}

static uint8_t AccGyrCtrl2Byte()
{
    uint8_t odr = AccGyr.SamplingRate < 8 ? AccGyrOdrCode[AccGyr.SamplingRate] : 0;
    uint8_t fs = AccGyr.RangeAng < 5 ? AccGyrAngFsNib[AccGyr.RangeAng] : 0;
    return (uint8_t)((odr << 4) | fs);
}

// Applies the block's ODR + full-scale selections to both sensors and verifies the
// registers took the values. Used at boot (so a restored persistent config is re-applied
// to the part) and by the range/ODR write triggers.
static bool ApplyAccGyrConfig()
{
    uint8_t ctrl1 = AccGyrCtrl1Byte();
    uint8_t ctrl2 = AccGyrCtrl2Byte();
    if (!LSM6DS3WriteReg(0x10, ctrl1)) return false;
    if (!LSM6DS3WriteReg(0x11, ctrl2)) return false;
    return true;
}

// Initialises the I2C master bus and configures the LSM6DS3 IMU. Mirrors the previously
// working driver (internal pullups, RC_FAST clock, pin reset, finite timeouts) and adds a
// forced I2C restart + retry, because the part occasionally needs one before it accepts
// configuration. The block's persisted ODR/ranges are re-applied at the end.
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
        .trans_queue_depth = 0, // synchronous transfers only
        .flags = {.enable_internal_pullup = true, .allow_pd = false},
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
        if (!ApplyAccGyrConfig())
            ESP_LOGW("LSM6DS3", "Re-applying block config failed");
    }
    else
    {
        ESP_LOGE("LSM6DS3", "Init failed after retries: WHO_AM_I=0x%02X err=0x%x", who, who_err);
        ReportAccGyrError(ErrInitFailed);
    }
}

// Applies the 2-norm deadzone (Docs/Modules and blocks/Measurement.md): the current output
// is the center; the new (filtered) value is adopted only when it moves more than `dz`
// away in Euclidean distance. 0 = off.
static Vector<3> AccGyrApplyDeadzone(const Vector<3> &next, const Vector<3> &prev, Number dz)
{
    if (dz <= N(0)) return next;
    if ((next - prev).norm2() <= dz) return prev;
    return next;
}

// Reads raw gyro/accel registers and updates the block outputs: per-range scaling, EMA
// (0-1 coefficient) on the raw values, then a 2-norm deadzone on the results.
bool ReadIMUData() {
    uint16_t Raw[6];

    // Read 12 bytes starting from register 0x22 (gyro X/Y/Z then accel X/Y/Z)
    if (LSM6DS3ReadRegs(0x22, (uint8_t *)Raw, 12) != ESP_OK) {
        ReportAccGyrError(ErrBusGeneric);
        return false;
    }

    // Scale factors per the selected full-scale. Derived from the datasheet sensitivities
    // (Table 51: 0.061/0.122/0.244/0.488 mg/LSb; Table 54: 4.375/8.75/17.5/35/70 mdps/LSb)
    // and anchored to the previously-calibrated defaults: accel 209 @ +-16 g, gyro 939 @
    // +-2000 dps, so the default ranges keep today's readings.
    uint8_t ri = AccGyr.RangeAcc > 3 ? 3 : AccGyr.RangeAcc;
    uint8_t gi = AccGyr.RangeAng > 4 ? 4 : AccGyr.RangeAng;
    static const Number accel_div[4] = {N(1672), N(836), N(418), N(209)};
    static const Number gyro_div[5] = {N(15024), N(7512), N(3756), N(1878), N(939)};

    // EMA coefficients (0-1); clamp so a remotely-written out-of-range value cannot make
    // the low-pass diverge. 1 = no filtering (output tracks the raw value).
    Number acc_w = AccGyr.AccFilter;
    if (acc_w < N(0)) acc_w = N(0);
    if (acc_w > N(1)) acc_w = N(1);
    Number ang_w = AccGyr.AngFilter;
    if (ang_w < N(0)) ang_w = N(0);
    if (ang_w > N(1)) ang_w = N(1);

    Vector<3> next_gyro, next_acc;
    for (int i = 0; i < 3; i++)
    {
        next_gyro.Data[i] = (Number(Raw[i]) / gyro_div[gi]) * ang_w +
                            (AccGyr.AngularVelocity.Data[i] * (N(1) - ang_w));
        next_acc.Data[i] = (Number(Raw[3 + i]) / accel_div[ri]) * acc_w +
                           (AccGyr.Acceleration.Data[i] * (N(1) - acc_w));
    }

    AccGyr.AngularVelocity = AccGyrApplyDeadzone(next_gyro, AccGyr.AngularVelocity, AccGyr.AngDeadzone);
    AccGyr.Acceleration = AccGyrApplyDeadzone(next_acc, AccGyr.Acceleration, AccGyr.AccDeadzone);

    return true;
}

// Writes one of the enum fields (index into the option table) into the block. Shared by
// the ODR and range triggers: clamps the index, applies the new register byte(s) with
// read-back verification, and only commits the block field once the part accepted it.
static bool AccGyrWriteEnum(const StaticBlockDescriptor &block, uint16_t field, const void *data, uint16_t data_len)
{
    if (data_len != sizeof(uint8_t)) return false;
    uint8_t index = *static_cast<const uint8_t *>(data);
    switch (field)
    {
    case 0: // Sampling Rate (ODR): clamps to [0, 7]
        if (index >= 8) index = 7;
        break;
    case 1: // Range Acceleration: clamps to [0, 3]
        if (index >= 4) index = 3;
        break;
    case 2: // Range Angular: clamps to [0, 4]
        if (index >= 5) index = 4;
        break;
    default:
        return false;
    }

    if (!ApplyAccGyrConfig())
        return false;

    switch (field)
    {
    case 0: AccGyr.SamplingRate = index; break;
    case 1: AccGyr.RangeAcc = index; break;
    case 2: AccGyr.RangeAng = index; break;
    }
    return true;
}

bool OnAccGyrFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    return AccGyrWriteEnum(block, 0, data, data_len);
}

bool OnAccGyrAccRangeChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    return AccGyrWriteEnum(block, 1, data, data_len);
}

bool OnAccGyrAngRangeChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len) {
    return AccGyrWriteEnum(block, 2, data, data_len);
}