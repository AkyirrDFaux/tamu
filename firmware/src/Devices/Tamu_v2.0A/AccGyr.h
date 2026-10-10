#pragma once

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "Core/Functions/Packet.h"
#include "Core/Services/LogHandler.h"

// Reports an accelerometer/gyroscope error to the log service.
static inline void ReportAccGyrError(AccGyrError Code) {
    ReportLog(MakeLog(true, (uint16_t)BlockType::AccGyr, (uint16_t)Code, 0), PRIORITY_ERROR);
}

static i2c_master_bus_handle_t I2cBusHandle;
static i2c_master_dev_handle_t Lsm6ds3Handle;

// Reads `size` bytes starting at register `reg`. Combined transmit(receive with a finite
// timeout, matching the previously-working I2C driver. Internal pullups + RC_FAST clock are
// required: without pullups the bus floats low and every read returns ESP_OK with zeros.
static esp_err_t LSM6DS3ReadRegs(uint8_t Reg, uint8_t *Data, uint8_t Size)
{
    return i2c_master_transmit_receive(Lsm6ds3Handle, &Reg, 1, Data, Size, 1000);
}

// Writes a single register and verifies the value reads back (writes into a stuck or
// mid-boot part are dropped silently, so the read-back is the source of truth).
static bool LSM6DS3WriteReg(uint8_t Reg, uint8_t Value)
{
    uint8_t Cmd[] = {Reg, Value};
    if (i2c_master_transmit(Lsm6ds3Handle, Cmd, 2, 1000) != ESP_OK)
        return false;
    uint8_t Check = 0;
    if (LSM6DS3ReadRegs(Reg, &Check, 1) != ESP_OK)
        return false;
    return Check == Value;
}

// Reads the WHO_AM_I register (0x0F); returns true when the LSM6DS3 answers and the value
// matches the LSM6DS3 family (0x69, some variants 0x6A).
static bool LSM6DS3ReadWho(uint8_t *WhoOut, esp_err_t *ErrOut)
{
    uint8_t Who = 0;
    esp_err_t Err = LSM6DS3ReadRegs(0x0F, &Who, 1);
    if (ErrOut) *ErrOut = Err;
    if (Err != ESP_OK)
        return false;
    if (WhoOut) *WhoOut = Who;
    return (Who == 0x69 || Who == 0x6A);
}

// Forces a restart of the LSM6DS3 over I2C (BOOT + SW_RESET in CTRL3_C, 0x12). BOOT
// re-initialises the part the same way a power cycle does, clearing the stuck state the
// sensor can occasionally get into after a cold boot (this board needs it sometimes).
static void LSM6DS3ForceRestart()
{
    uint8_t BootCmd[] = {0x12, 0b10000001};
    i2c_master_transmit(Lsm6ds3Handle, BootCmd, 2, 1000);
    vTaskDelay(pdMS_TO_TICKS(30));
}

// Configures the accel (CTRL1_XL) and gyro (CTRL2_G) ODR/full-scale registers and verifies
// they read back the values just written. IF_INC (CTRL3_C bit 2) defaults to 1 after reset on
// this part, so the multi-byte config write steps 0x10 -> 0x11 and the 12-byte sensor read in
// ReadIMUData steps 0x22..0x2D. The IF_INC write is kept explicit for robustness.
static bool LSM6DS3Configure()
{
    // Enable address auto-increment (IF_INC, CTRL3_C bit 2).
    uint8_t IfincCmd[] = {0x12, 0b00000100};
    if (i2c_master_transmit(Lsm6ds3Handle, IfincCmd, 2, 1000) != ESP_OK)
        return false;

    // Consecutive Write: 0x10 (CTRL1_XL, accel 104 Hz +/-16 g) and 0x11 (CTRL2_G, gyro 104 Hz +/-2000 dps).
    // FS_XL = 01 -> +/-16 g and FS_G = 11 -> +/-2000 dps per the LSM6DS3TR datasheet
    // (Tables 51/54); the raw divisors in ReadIMUData are calibrated for these settings.
    uint8_t ConfigCmd[] = {0x10, 0b01000100, 0b01001100};
    if (i2c_master_transmit(Lsm6ds3Handle, ConfigCmd, 3, 1000) != ESP_OK)
        return false;

    // Verify the ODR registers actually stuck (writes into a stuck/mid-boot part are dropped).
    uint8_t Ctrl1 = 0, Ctrl2 = 0;
    if (LSM6DS3ReadRegs(0x10, &Ctrl1, 1) != ESP_OK) return false;
    if (LSM6DS3ReadRegs(0x11, &Ctrl2, 1) != ESP_OK) return false;
    return (Ctrl1 == 0b01000100 && Ctrl2 == 0b01001100);
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
    uint8_t Odr = staticPer.Accgyr.SamplingRate < 8 ? AccGyrOdrCode[staticPer.Accgyr.SamplingRate] : 0;
    uint8_t Fs = staticPer.Accgyr.RangeAcc < 4 ? AccGyrAccFsCode[staticPer.Accgyr.RangeAcc] : 0;
    return (uint8_t)((Odr << 4) | (Fs << 2));
}

static uint8_t AccGyrCtrl2Byte()
{
    uint8_t Odr = staticPer.Accgyr.SamplingRate < 8 ? AccGyrOdrCode[staticPer.Accgyr.SamplingRate] : 0;
    uint8_t Fs = staticPer.Accgyr.RangeAng < 5 ? AccGyrAngFsNib[staticPer.Accgyr.RangeAng] : 0;
    return (uint8_t)((Odr << 4) | Fs);
}

// Applies the block's ODR + full-scale selections to both sensors and verifies the
// registers took the values. Used at boot (so a restored persistent config is re-applied
// to the part) and by the range/ODR write triggers.
static bool ApplyAccGyrConfig()
{
    uint8_t Ctrl1 = AccGyrCtrl1Byte();
    uint8_t Ctrl2 = AccGyrCtrl2Byte();
    if (!LSM6DS3WriteReg(0x10, Ctrl1)) return false;
    if (!LSM6DS3WriteReg(0x11, Ctrl2)) return false;
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
    if (!AppInterface.Connected)
        ESP_LOGW("LSM6DS3", "Bus lines with pullup: SDA(GPIO4)=%d SCL(GPIO5)=%d",
                 gpio_get_level((gpio_num_t)GPIO_NUM_4), gpio_get_level((gpio_num_t)GPIO_NUM_5));

    // 2. Initialize I2C Master Bus. The board carries 4k7 external pull-ups on SDA/SCL, and
    //    the internal pull-ups are kept enabled as well (belt and braces): a missing or
    //    wrongly-populated external part then cannot leave the bus floating low, where every
    //    transaction "succeeds" (ESP_OK) but reads return all zeros.
    i2c_master_bus_config_t BusCfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_5,
        .clk_source = I2C_CLK_SRC_RC_FAST,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0, // synchronous transfers only
        .flags = {.enable_internal_pullup = true, .allow_pd = false},
    };
    if (i2c_new_master_bus(&BusCfg, &I2cBusHandle) != ESP_OK) {
        ReportAccGyrError(ErrBusGeneric);
        return;
    }

    // 3. Add LSM6DS3TRC Device (address 0x6A)
    i2c_device_config_t DevCfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6A,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {},
    };
    if (i2c_master_bus_add_device(I2cBusHandle, &DevCfg, &Lsm6ds3Handle) != ESP_OK) {
        ReportAccGyrError(ErrDeviceNotFound);
        return;
    }

    bool Configured = false;
    uint8_t Who = 0;
    esp_err_t WhoErr = ESP_OK;

    // First try without forcing a restart (the sensor is usually healthy); force it only
    // when WHO_AM_I is absent, then re-verify the configuration afterwards.
    for (int Attempt = 0; Attempt < 5 && !Configured; Attempt++)
    {
        if (Attempt > 0)
            LSM6DS3ForceRestart();

        if (!LSM6DS3ReadWho(&Who, &WhoErr))
            continue; // unresponsive / wrong WHO_AM_I; force another restart

        Configured = LSM6DS3Configure();
    }

    if (Configured)
    {
        if (!AppInterface.Connected)
            ESP_LOGI("LSM6DS3", "Configured (WHO_AM_I=0x%02X).", Who);
        if (!ApplyAccGyrConfig() && !AppInterface.Connected)
            ESP_LOGW("LSM6DS3", "Re-applying block config failed");
    }
    else
    {
        if (!AppInterface.Connected)
            ESP_LOGE("LSM6DS3", "Init failed after retries: WHO_AM_I=0x%02X err=0x%x", Who, WhoErr);
        ReportAccGyrError(ErrInitFailed);
    }
}

// Reads raw gyro/accel registers and updates the block outputs: per-range scaling and
// EMA (0-1 coefficient) on the raw values.
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
    uint8_t Ri = staticPer.Accgyr.RangeAcc > 3 ? 3 : staticPer.Accgyr.RangeAcc;
    uint8_t Gi = staticPer.Accgyr.RangeAng > 4 ? 4 : staticPer.Accgyr.RangeAng;
    static const Number AccelDiv[4] = {N(1672), N(836), N(418), N(209)};
    static const Number GyroDiv[5] = {N(15024), N(7512), N(3756), N(1878), N(939)};

    // EMA coefficients (0-1); clamp so a remotely-written out-of-range value cannot make
    // the low-pass diverge. 1 = no filtering (output tracks the raw value).
    Number AccW = staticPer.Accgyr.AccFilter;
    if (AccW < N(0)) AccW = N(0);
    if (AccW > N(1)) AccW = N(1);
    Number AngW = staticPer.Accgyr.AngFilter;
    if (AngW < N(0)) AngW = N(0);
    if (AngW > N(1)) AngW = N(1);

    Vector<3> NextGyro, NextAcc;
    for (int I = 0; I < 3; I++)
    {
        NextGyro.Data[I] = (Number(Raw[I]) / GyroDiv[Gi]) * AngW +
                            (staticVol.Accgyr.AngularVelocity.Data[I] * (N(1) - AngW));
        NextAcc.Data[I] = (Number(Raw[3 + I]) / AccelDiv[Ri]) * AccW +
                           (staticVol.Accgyr.Acceleration.Data[I] * (N(1) - AccW));
    }

    staticVol.Accgyr.AngularVelocity = NextGyro;
    staticVol.Accgyr.Acceleration = NextAcc;

    return true;
}

// Writes one of the enum fields (index into the option table) into the block. Shared by
// the ODR and range triggers: clamps the index, commits it so ApplyAccGyrConfig builds the
// register bytes from the NEW selection, then reverts the field if the part rejected the
// write (read-back verified).
static bool AccGyrWriteEnum(const StaticBlockDescriptor &Block, uint16_t Field, const void *Data, uint16_t DataLen)
{
    if (DataLen != sizeof(uint8_t)) return false;
    uint8_t Index = *static_cast<const uint8_t *>(Data);
    uint8_t *Target = nullptr;
    switch (Field)
    {
    case 0: // Sampling Rate (ODR): clamps to [0, 7]
        if (Index >= 8) Index = 7;
        Target = &staticPer.Accgyr.SamplingRate;
        break;
    case 1: // Range Acceleration: clamps to [0, 3]
        if (Index >= 4) Index = 3;
        Target = &staticPer.Accgyr.RangeAcc;
        break;
    case 2: // Range Angular: clamps to [0, 4]
        if (Index >= 5) Index = 4;
        Target = &staticPer.Accgyr.RangeAng;
        break;
    default:
        return false;
    }

    // Commit the clamped index BEFORE applying: ApplyAccGyrConfig builds the CTRL1/CTRL2
    // bytes from the block fields, so applying first would program the OLD selection while
    // the field reports the new one. Revert the field if the part rejected the write.
    uint8_t Saved = *Target;
    *Target = Index;
    if (!ApplyAccGyrConfig())
    {
        *Target = Saved;
        return false;
    }
    return true;
}

bool OnAccGyrFrequencyChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen) {
    return AccGyrWriteEnum(Block, 0, Data, DataLen);
}

bool OnAccGyrAccRangeChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen) {
    return AccGyrWriteEnum(Block, 1, Data, DataLen);
}

bool OnAccGyrAngRangeChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen) {
    return AccGyrWriteEnum(Block, 2, Data, DataLen);
}