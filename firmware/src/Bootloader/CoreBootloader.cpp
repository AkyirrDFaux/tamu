// Core (ESP32-C3) bootloader (Docs/Services/Bootloader.md): a small `factory` app that runs
// first and either stays in update mode or boots the main app from `ota_0`.
//
// Entry (B1): this app always runs first. It reads the board button (GPIO2, active low); if
// it is held it serves raw bootloader frames over the USB Serial/JTAG port, otherwise it
// selects `ota_0` and restarts. The main app re-arms `otadata = factory` at startup so this
// app keeps running first on the next reset. Updates only ever write `ota_0`, so `factory`
// stays intact and the device is always recoverable.
//
// The bootloader is deliberately dumb: raw write/read only, no image parsing. The app owns
// the flow (write -> read back -> correct) and the verification.

#include <cstdint>
#include <cstring>

#include "Core/Functions/Bootloader.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The board's LED-button (Docs/Devices.md): GPIO2, pull-up, active low.
#define BUTTON_PIN GPIO_NUM_2

// ESP32-C3 flash sector: `ota_0` is erased a sector at a time (the RSBus/DAS 64-byte page
// granularity does not apply here).
#define SECTOR_SIZE 4096u

// The main-app slot (found at boot).
static const esp_partition_t *s_app = nullptr;

// Which `ota_0` sectors have already been erased this session. The app writes sequentially
// and may re-write (correct) a chunk, so a sector must be erased once - on its first write -
// or a correction of the sector's first chunk would wipe the rest of the sector.
static uint8_t s_erased[0x2A0000u / SECTOR_SIZE / 8u];

static bool SectorErased(uint32_t sector)
{
    return (s_erased[sector >> 3] & (uint8_t)(1u << (sector & 7u))) != 0;
}

static void MarkErased(uint32_t sector)
{
    s_erased[sector >> 3] |= (uint8_t)(1u << (sector & 7u));
}

// ---------------------------------------------------------------------------
// USB (raw bootloader frames over the USB Serial/JTAG port)
// ---------------------------------------------------------------------------

static void SendRaw(const uint8_t *data, size_t len)
{
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
}

static bool ReadByte(uint8_t *b, TickType_t ticks)
{
    return usb_serial_jtag_read_bytes(b, 1, ticks) == 1;
}

// Reads one raw frame into `out`; returns its length or 0 on timeout/invalid.
static int ReceiveFrame(uint8_t *out, size_t cap, TickType_t ticks)
{
    uint8_t b;
    for (;;)
    {
        if (!ReadByte(&b, ticks)) return 0;
        if (b == Bootloader::START) break; // skip anything that is not a frame start
    }

    size_t got = 0;
    out[got++] = b;
    if (!ReadByte(&b, ticks)) return 0;
    out[got++] = b;

    uint16_t need = Bootloader::FrameSize((uint8_t)(b & 0x03u));
    if (need == 0 || need > cap) return 0;
    while (got < need)
    {
        if (!ReadByte(&b, ticks)) return 0;
        out[got++] = b;
    }
    return Bootloader::Decode(out, need) ? (int)need : 0;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void HandleWrite(uint32_t offset, const uint8_t *payload)
{
    if (s_app == nullptr) return;
    if ((offset & 31u) != 0) return; // 32-byte aligned
    if (offset + Bootloader::PAYLOAD_SIZE > s_app->size) return;

    uint32_t sector = offset / SECTOR_SIZE;
    if (!SectorErased(sector))
    {
        if (esp_partition_erase_range(s_app, sector * SECTOR_SIZE, SECTOR_SIZE) != ESP_OK)
            return;
        MarkErased(sector);
    }
    esp_partition_write(s_app, offset, payload, Bootloader::PAYLOAD_SIZE);
}

static void HandleRead(uint32_t offset)
{
    uint8_t data[Bootloader::PAYLOAD_SIZE];
    if (s_app == nullptr || offset + Bootloader::PAYLOAD_SIZE > s_app->size)
        memset(data, 0xFF, sizeof(data));
    else
        esp_partition_read(s_app, offset, data, sizeof(data));

    uint8_t resp[Bootloader::DATA_SIZE];
    Bootloader::EncodeReadResponse(offset, data, resp);
    SendRaw(resp, sizeof(resp));
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << BUTTON_PIN;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&button);

    const esp_partition_t *main_app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);

    // Let the pull-up settle, then sample: released (high) boots the main app.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (gpio_get_level(BUTTON_PIN) != 0 && main_app != nullptr)
    {
        // Boot only when `ota_0` holds an app image (its header magic byte 0xE9); otherwise
        // stay in update mode, or a fresh device would reboot-loop through this app.
        uint8_t magic = 0xFF;
        esp_partition_read(main_app, 0, &magic, 1);
        if (magic == 0xE9)
        {
            esp_ota_set_boot_partition(main_app);
            esp_restart();
        }
    }

    // Update mode: own the USB port and serve raw frames.
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb);
    s_app = main_app;

    for (;;)
    {
        uint8_t frame[Bootloader::MAX_FRAME_SIZE];
        int n = ReceiveFrame(frame, sizeof(frame), pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        uint8_t cmd = (uint8_t)(frame[1] & 0x03u);
        uint32_t offset = Bootloader::Offset(frame);
        if (cmd == Bootloader::CMD_WRITE)
            HandleWrite(offset, Bootloader::Payload(frame));
        else if (cmd == Bootloader::CMD_READ_REQ)
            HandleRead(offset);
    }
}
