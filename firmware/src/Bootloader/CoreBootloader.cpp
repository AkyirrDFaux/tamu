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
#include "Core/Functions/BootloaderHost.h"
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
static const esp_partition_t *App = nullptr;

// Which `ota_0` sectors have already been erased in the current write pass. The app writes
// sequentially and may re-write (correct) a chunk, so a sector must be erased once - on its
// first write - or a correction of the sector's first chunk would wipe the rest of the
// sector. A new pass starts when the app writes offset 0 again, which resets the bitmap.
//
// The size must cover `ota_0` as allocated in `partitions.csv` (0x2A0000); `app_main`
// refuses writes when the runtime partition is larger, so the sector index cannot run off
// the end of this array.
static constexpr uint32_t OTA_APP_SIZE = 0x2A0000u;
static uint8_t Erased[OTA_APP_SIZE / SECTOR_SIZE / 8u];
static_assert(OTA_APP_SIZE % SECTOR_SIZE == 0, "ota_0 size must be a whole number of sectors");

static bool SectorErased(uint32_t Sector)
{
    return (Erased[Sector >> 3] & (uint8_t)(1u << (Sector & 7u))) != 0;
}

static void MarkErased(uint32_t Sector)
{
    Erased[Sector >> 3] |= (uint8_t)(1u << (Sector & 7u));
}

// ---------------------------------------------------------------------------
// USB (raw bootloader frames over the USB Serial/JTAG port)
// ---------------------------------------------------------------------------

static void SendRaw(const uint8_t *Data, size_t Len)
{
    usb_serial_jtag_write_bytes(Data, Len, pdMS_TO_TICKS(1000));
}

static bool ReadByte(uint8_t *B, TickType_t Ticks)
{
    return usb_serial_jtag_read_bytes(B, 1, Ticks) == 1;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// Offset of the last successfully programmed chunk (0xFFFFFFFF = nothing yet). Reported
// back on every write so the app resynchronises after a rejected chunk instead of guessing.
static uint32_t LastWritten = 0xFFFFFFFFu;

static void HandleWrite(uint32_t Offset, const uint8_t *Payload)
{
    // Compute success without early returns: every write - accepted or rejected - is
    // acknowledged with a write response carrying LastWritten, so the app can recover.
    bool Ok = (App != nullptr);
    if (Ok && (Offset & 31u) != 0) Ok = false; // 32-byte aligned
    // `offset + PAYLOAD_SIZE` wraps near 2^32, so subtract instead: the sector index below
    // must stay inside `Erased`.
    if (Ok && (Offset > App->size || App->size - Offset < Bootloader::PAYLOAD_SIZE)) Ok = false;

    if (Ok)
    {
        // A write pass starts at offset 0 (the app writes the image from the beginning,
        // including a retry after a failed verification), so forget the previous pass's
        // erased sectors. A correction that re-writes offset 0 is safe: the bitmap is reset
        // before that sector is erased, and the rest of the pass programs into the freshly
        // erased sector.
        if (Offset == 0) memset(Erased, 0, sizeof(Erased));

        uint32_t Sector = Offset / SECTOR_SIZE;
        if (!SectorErased(Sector))
        {
            if (esp_partition_erase_range(App, Sector * SECTOR_SIZE, SECTOR_SIZE) != ESP_OK)
                Ok = false;
            else
                MarkErased(Sector);
        }
        if (Ok)
        {
            esp_partition_write(App, Offset, Payload, Bootloader::PAYLOAD_SIZE);
            LastWritten = Offset;
        }
    }

    uint8_t Resp[Bootloader::READ_REQ_SIZE];
    Bootloader::EncodeWriteResponse(LastWritten, Resp);
    SendRaw(Resp, sizeof(Resp));
}

// The identify response is entirely build-time constant (app size + name), so the whole
// frame - control/parity included - is precomputed and lives in flash. Nothing is built at
// runtime: the handler just sends the constant.
struct IdentifyFrame { uint8_t B[Bootloader::DATA_SIZE]; };

constexpr IdentifyFrame MakeIdentify(uint32_t Space, const char *Name)
{
    IdentifyFrame f{};
    f.B[0] = Bootloader::START;
    f.B[1] = Bootloader::CMD_IDENTIFY_RESP;
    f.B[2] = (uint8_t)(Space & 0xFFu);
    f.B[3] = (uint8_t)((Space >> 8) & 0xFFu);
    f.B[4] = (uint8_t)((Space >> 16) & 0xFFu);
    f.B[5] = (uint8_t)((Space >> 24) & 0xFFu);
    for (uint8_t i = 0; i < Bootloader::NAME_SIZE; i++)
    {
        if (Name[i] == '\0') break;
        f.B[6 + i] = (uint8_t)Name[i];
    }
    f.B[Bootloader::DATA_SIZE - 1] = Bootloader::END;
    f.B[1] |= (uint8_t)(Bootloader::Parity(f.B, Bootloader::DATA_SIZE) << 3);
    return f;
}

constexpr IdentifyFrame Identify = MakeIdentify(OTA_APP_SIZE, "Tamu v2.0A");

static void HandleIdentify() { SendRaw(Identify.B, Bootloader::DATA_SIZE); }

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    gpio_config_t Button = {};
    Button.pin_bit_mask = 1ULL << BUTTON_PIN;
    Button.mode = GPIO_MODE_INPUT;
    Button.pull_up_en = GPIO_PULLUP_ENABLE;
    Button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    Button.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&Button);

    const esp_partition_t *MainApp = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);

    // Let the pull-up settle, then sample: released (high) boots the main app.
    vTaskDelay(pdMS_TO_TICKS(2));
#ifndef TAMU_BOOTLOADER_FORCE
    // TAMU_BOOTLOADER_FORCE (test hook) skips the button check so the USB update path can be
    // exercised without physically holding GPIO2.
    if (gpio_get_level(BUTTON_PIN) != 0 && MainApp != nullptr)
    {
        // Boot only when `ota_0` holds an app image (its header magic byte 0xE9); otherwise
        // stay in update mode, or a fresh device would reboot-loop through this app.
        uint8_t Magic = 0xFF;
        esp_partition_read(MainApp, 0, &Magic, 1);
        if (Magic == 0xE9)
        {
            esp_ota_set_boot_partition(MainApp);
            esp_restart();
        }
    }
#endif

    // Update mode: own the USB port and serve raw frames.
    usb_serial_jtag_driver_config_t Usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&Usb);
    App = MainApp;
    // Guard the sector bitmap against a `partitions.csv` size change: a larger `ota_0` would
    // make the sector index run past `s_erased`. Refuse writes (reads return 0xFF) rather
    // than corrupt memory.
    if (App != nullptr && App->size > OTA_APP_SIZE)
        App = nullptr;

    // Serve raw frames (the receive/read/dispatch plumbing is shared with the DAS host).
    BootloaderHost::HostLoop(
        [] {},
        [](uint8_t *B) { return ReadByte(B, pdMS_TO_TICKS(100)); },
        [](uint32_t Offset, const uint8_t *Payload) { HandleWrite(Offset, Payload); },
        [](uint32_t Offset) {
            auto readPayload = [](uint32_t Off, uint8_t *Data) {
                // Same wrap-safe bounds check as `HandleWrite`: an out-of-range offset
                // reads back 0xFF (erased) instead of leaking stack bytes.
                if (App == nullptr || Off > App->size ||
                    App->size - Off < Bootloader::PAYLOAD_SIZE)
                    return false;
                esp_partition_read(App, Off, Data, Bootloader::PAYLOAD_SIZE);
                return true;
            };
            auto Send = [](const uint8_t *D, size_t N) { SendRaw(D, N); };
            BootloaderHost::HandleRead(Offset, readPayload, Send);
        },
        [] { HandleIdentify(); });
}
