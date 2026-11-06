// Core (ESP32-C3) factory image (Docs/Services/Bootloader.md): a small app that runs first and
// either serves an update or boots the main app from `ota_0`.
//
// Entry (B1): this app always runs first. It reads the board button (GPIO2, active low); if it
// is held it serves raw bootloader frames, otherwise it selects `ota_0` and restarts. The main
// app erases `otadata` at startup (its own liveness proof) so this app keeps running first on the
// next reset. Updates only ever write `ota_0`, so `factory` stays intact.
//
// Two transports, one frame codec: the USB Serial/JTAG port of the original image, plus a
// pairing-free BLE transport (NimBLE host+controller, one service with a write and a notify
// characteristic, no security manager, no bonds). Whichever an update arrives on, the bytes go
// through the same parser and the same write/read handlers, so the two cannot disagree about
// what a frame is.
//
// Recovery, and why this image needs a boot budget as well as the button:
//   - an image that never lands (magic not 0xE9) is never booted;
//   - an image whose body crashes before it proves itself is caught by the second-stage
//     bootloader's pending-verify/rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): this app
//     selects `ota_0` with state NEW, the bootloader marks it PENDING_VERIFY, and the main app's
//     otadata erase marks it valid. A crash before that erase leaves it unconfirmed, so the next
//     reset rolls back to this partition and `ota_0` is marked ABORTED - which this app refuses
//     to boot (BootDecision.h), i.e. it stays here with the updater on the air;
//   - an image that does prove itself and then dies later is caught by the attempt budget: every
//     boot spends one attempt, the app's otadata erase refunds it, and a spent budget keeps the
//     device here (BootDecision.h).
// Both paths end in update mode, which is reachable without a cable (BLE, or B1 for a device
// that is not on the air) - the case the old "boot anything with a 0xE9" check could not cover.

#include <cstdint>
#include <cstring>

#include "Bootloader/BootBudget.h"
#include "Bootloader/BootDecision.h"
#include "Bootloader/UpdaterAdmission.h"
#include "Core/Functions/Bootloader.h"
#include "Core/Functions/BootloaderHost.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// The board's LED-button (Docs/Devices.md): GPIO2, pull-up, active low. The button and the
// notification LED share the pin. Update mode lights it as its boot signal (the Tamu v2.0A has
// no white LED).
#define BUTTON_PIN GPIO_NUM_2
#define LED_PIN BUTTON_PIN

// ESP32-C3 flash sector: `ota_0` is erased a sector at a time (the RSBus/DAS 64-byte page
// granularity does not apply here).
static constexpr uint32_t SECTOR_SIZE = 4096u;

// `ota_0` as allocated in `partitions.csv` (0x283000 at 0x90000). The sector bitmap below has
// to cover it, and the identify response reports it as the writable app space. A runtime
// partition larger than this would make the sector index run past the array, so writes are
// refused in that case (see app_main) rather than corrupting memory.
static constexpr uint32_t OTA_APP_SIZE = 0x283000u;

static constexpr uint8_t IMAGE_MAGIC = 0xE9;
static_assert(OTA_APP_SIZE % SECTOR_SIZE == 0, "ota_0 size must be a whole number of sectors");

static const char *TAG = "UPDATER";

// The main-app slot (found at boot).
static const esp_partition_t *App = nullptr;

// Which `ota_0` sectors have already been erased in the current write pass. The app writes
// sequentially and may re-write (correct) a chunk, so a sector must be erased once - on its
// first write - or a correction of the sector's first chunk would wipe the rest of the sector.
// A new pass starts when the app writes offset 0 again, which resets the bitmap.
static uint8_t Erased[OTA_APP_SIZE / SECTOR_SIZE / 8u];

static bool SectorErased(uint32_t Sector)
{
    return (Erased[Sector >> 3] & (uint8_t)(1u << (Sector & 7u))) != 0;
}

static void MarkErased(uint32_t Sector)
{
    Erased[Sector >> 3] |= (uint8_t)(1u << (Sector & 7u));
}

// Offset of the last successfully programmed chunk (0xFFFFFFFF = nothing yet). Reported back on
// every write so the app resynchronises after a rejected chunk instead of guessing.
static uint32_t LastWritten = 0xFFFFFFFFu;
static uint8_t FailedBoots = 0;

static uint32_t NowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// Boot budget (NVS, shared with the main app through BootBudget.h)
// ---------------------------------------------------------------------------

// Erasing `otadata` with `esp_ota_set_boot_partition` is what the main app does on every boot:
// an image that reaches its own first statement has proved it can run, and the erase leaves the
// next reset defaulting to this partition (so B1 is always catchable). The same erase here
// forgets a rollback: a freshly written image must be bootable again even if the previous image
// in that slot was marked ABORTED.
static void ClearOtadata()
{
    const esp_partition_t *Ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (Ota != nullptr)
        esp_partition_erase_range(Ota, 0, Ota->size);
}

// An image is present when the slot starts with the ESP image magic. That is all the old image
// checked; the state check below is what stops a crash-before-it-proves-itself image from being
// re-armed for ever.
static bool ImagePresent(const esp_partition_t *Part)
{
    if (Part == nullptr)
        return false;
    uint8_t Magic = 0xFF;
    return esp_partition_read(Part, 0, &Magic, 1) == ESP_OK && Magic == IMAGE_MAGIC;
}

// True when the bootloader has rolled this slot back (or it was marked invalid): the image in it
// never proved it could run, so booting it again would only repeat the crash.
static bool ImageRolledBack(const esp_partition_t *Part)
{
    if (Part == nullptr)
        return false;
    esp_ota_img_states_t State;
    return esp_ota_get_state_partition(Part, &State) == ESP_OK &&
           (State == ESP_OTA_IMG_ABORTED || State == ESP_OTA_IMG_INVALID);
}

// ---------------------------------------------------------------------------
// BLE transport: one custom service, write (peer -> device) + notify (device -> peer)
// ---------------------------------------------------------------------------

// UUIDs are the byte arrays NimBLE wants (little-endian), so these read as
//   54414D55-0001-4B4C-8000-00805F9B34FB  service
//   54414D55-0002-...                     frame in  (peer writes, no response)
//   54414D55-0003-...                     frame out (device notifies)
static const ble_uuid128_t SvcUuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x4c, 0x4b, 0x01, 0x00, 0x55, 0x4d, 0x41, 0x54);
static const ble_uuid128_t RxUuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x4c, 0x4b, 0x02, 0x00, 0x55, 0x4d, 0x41, 0x54);
static const ble_uuid128_t TxUuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x4c, 0x4b, 0x03, 0x00, 0x55, 0x4d, 0x41, 0x54);

static uint16_t TxHandle;
static volatile uint16_t ConnHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool NotifyOn = false;
static volatile uint32_t WindowEndMs = 0;
static uint8_t OwnAddrType;

// Peer -> device bytes. Written by the NimBLE host task (GATT access callback), read by the app
// task's loop; one producer, one consumer, so the 16-bit indices need no lock.
static uint8_t RxRing[1024];
static volatile uint16_t RxHead = 0;
static volatile uint16_t RxTail = 0;

static bool WindowIsOpen()
{
    return UpdaterAdmission::WindowOpen(NowMs(), WindowEndMs);
}

static void RxPush(const uint8_t *Data, uint16_t Len)
{
    for (uint16_t i = 0; i < Len; i++)
    {
        const uint16_t Next = (uint16_t)((RxHead + 1) & (sizeof(RxRing) - 1));
        if (Next == RxTail)
            return; // full: the rest of this write is dropped, the app's retry re-sends
        RxRing[RxHead] = Data[i];
        RxHead = Next;
    }
}

static bool RxPop(uint8_t *Out)
{
    if (RxHead == RxTail)
        return false;
    *Out = RxRing[RxTail];
    RxTail = (uint16_t)((RxTail + 1) & (sizeof(RxRing) - 1));
    return true;
}

static void SendFrame(const uint8_t *Data, size_t Len)
{
    // USB: the same bytes the USB-only image sent, so a USB host sees no difference.
    usb_serial_jtag_write_bytes(Data, Len, pdMS_TO_TICKS(100));

    const uint16_t Conn = ConnHandle;
    if (Conn == BLE_HS_CONN_HANDLE_NONE || !NotifyOn)
        return;
    struct os_mbuf *Om = os_msys_get_pkthdr(Len, 0);
    if (Om == nullptr)
        return;
    if (os_mbuf_append(Om, Data, Len) != 0)
    {
        os_mbuf_free_chain(Om);
        return;
    }
    ble_gatts_notify_custom(Conn, TxHandle, Om); // the stack owns `Om` from here
}

static int ChrAccess(uint16_t Conn, uint16_t Attr, struct ble_gatt_access_ctxt *Ctxt, void *Arg)
{
    (void)Conn;
    (void)Attr;
    (void)Arg;
    if (Ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    // No pairing and no passphrase, so the window is the whole admission decision: a frame that
    // arrives after it closed is refused at the attribute.
    if (!UpdaterAdmission::WriteAllowed(WindowIsOpen(), ConnHandle != BLE_HS_CONN_HANDLE_NONE))
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    const uint16_t Len = OS_MBUF_PKTLEN(Ctxt->om);
    if (Len == 0 || Len > 244) // 244 = ATT MTU 247 minus the 3-byte notification header
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t Buf[244];
    uint16_t Copied = 0;
    if (ble_hs_mbuf_to_flat(Ctxt->om, Buf, sizeof(Buf), &Copied) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    RxPush(Buf, Copied);
    return 0;
}

static const struct ble_gatt_svc_def GattSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SvcUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &RxUuid.u,
                .access_cb = ChrAccess,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &TxUuid.u,
                .access_cb = ChrAccess,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &TxHandle,
            },
            {0},
        },
    },
    {0},
};

static void Advertise();

static int GapEvent(struct ble_gap_event *Event, void *Arg)
{
    (void)Arg;
    switch (Event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (Event->connect.status != 0)
        {
            Advertise();
            return 0;
        }
        // One writer at a time, and only while the window is open: a peer that connected just
        // as the window closed is dropped rather than served.
        if (ConnHandle != BLE_HS_CONN_HANDLE_NONE || !WindowIsOpen())
        {
            ble_gap_terminate(Event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ConnHandle = Event->connect.conn_handle;
        NotifyOn = false;
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ConnHandle = BLE_HS_CONN_HANDLE_NONE;
        NotifyOn = false;
        Advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (Event->subscribe.attr_handle == TxHandle)
            NotifyOn = Event->subscribe.cur_notify != 0;
        return 0;

    default:
        return 0;
    }
}

static void Advertise()
{
    if (!WindowIsOpen() || ble_gap_adv_active())
        return;

    // Legacy advertising data is 31 bytes. Flags (3) + a complete 128-bit service UUID (18) is 21,
    // so the name cannot go here as well - it would make 35 and the call would fail with
    // BLE_HS_EMSGSIZE, leaving the device silent (measured on the rig, card t_3d74f8d1). The UUID
    // is what a phone filters on, so it goes in the advertisement and the name in the scan
    // response, where it costs the same 31-byte budget.
    struct ble_hs_adv_fields Fields = {};
    Fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    Fields.uuids128 = &SvcUuid;
    Fields.num_uuids128 = 1;
    Fields.uuids128_is_complete = 1;
    const int SetErr = ble_gap_adv_set_fields(&Fields);
    if (SetErr != 0)
    {
        ESP_LOGE(TAG, "advertising fields refused: %d", SetErr);
        return;
    }

    static const char Name[] = "Tamu updater";
    struct ble_hs_adv_fields Rsp = {};
    Rsp.name = (const uint8_t *)Name;
    Rsp.name_len = (uint8_t)(sizeof(Name) - 1);
    Rsp.name_is_complete = 1;
    const int RspErr = ble_gap_adv_rsp_set_fields(&Rsp);
    if (RspErr != 0)
        ESP_LOGW(TAG, "scan response refused: %d", RspErr);

    struct ble_gap_adv_params Params = {};
    Params.conn_mode = BLE_GAP_CONN_MODE_UND;
    Params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int StartErr = ble_gap_adv_start(OwnAddrType, nullptr, BLE_HS_FOREVER, &Params, GapEvent, nullptr);
    if (StartErr != 0)
        ESP_LOGE(TAG, "advertising start refused: %d", StartErr);
}

static void OnSync()
{
    const int AddrErr = ble_hs_util_ensure_addr(0);
    if (AddrErr != 0)
    {
        ESP_LOGE(TAG, "no usable own address: %d", AddrErr);
        return;
    }
    const int InferErr = ble_hs_id_infer_auto(0, &OwnAddrType);
    if (InferErr != 0)
    {
        ESP_LOGE(TAG, "address type inference failed: %d", InferErr);
        return;
    }
    Advertise();
}

static void OnReset(int Reason) { ESP_LOGW(TAG, "BLE host reset: %d", Reason); }

static void HostTask(void *Param)
{
    (void)Param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void BleStart()
{
    esp_err_t Err = nimble_port_init();
    if (Err != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", (int)Err);
        return;
    }
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = OnReset;
    // No ble_store_config_init(): the updater holds no bonds, and the store config is compiled
    // out with CONFIG_BT_NIMBLE_NVS_PERSIST. The window is the only admission rule.
    Err = ble_gatts_count_cfg(GattSvcs);
    if (Err != 0)
    {
        ESP_LOGE(TAG, "gatt count_cfg failed: %d", (int)Err);
        return;
    }
    Err = ble_gatts_add_svcs(GattSvcs);
    if (Err != 0)
    {
        ESP_LOGE(TAG, "gatt add_svcs failed: %d", (int)Err);
        return;
    }
    nimble_port_freertos_init(HostTask);
}

// ---------------------------------------------------------------------------
// Frames (Docs/Services/Bootloader.md) - dispatched from either transport
// ---------------------------------------------------------------------------

static void HandleWrite(uint32_t Offset, const uint8_t *Payload)
{
    // Compute success without early returns: every write - accepted or rejected - is
    // acknowledged with a write response carrying LastWritten, so the app can recover.
    bool Ok = (App != nullptr);
    if (Ok && (Offset & 31u) != 0)
        Ok = false; // 32-byte aligned
    // `offset + PAYLOAD_SIZE` wraps near 2^32, so subtract instead: the sector index below must
    // stay inside `Erased`.
    if (Ok && (Offset > App->size || App->size - Offset < Bootloader::PAYLOAD_SIZE))
        Ok = false;

    if (Ok)
    {
        if (Offset == 0)
        {
            // A write pass starts at offset 0 (the app writes the image from the beginning,
            // including a retry after a failed verification). Forget the previous pass's erased
            // sectors, give the new image its full boot budget, and forget a rollback so the
            // image about to be written is bootable again.
            memset(Erased, 0, sizeof(Erased));
            FailedBoots = UpdaterBoot::HealthyBoots();
            UpdaterBoot::StoreFailedBoots(FailedBoots);
            ClearOtadata();
        }
        const uint32_t Sector = Offset / SECTOR_SIZE;
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
    SendFrame(Resp, sizeof(Resp));
}

static void HandleRead(uint32_t Offset)
{
    auto ReadPayload = [](uint32_t Off, uint8_t *Data) {
        // Same wrap-safe bounds check as `HandleWrite`: an out-of-range offset reads back 0xFF
        // (erased) instead of leaking stack bytes.
        if (App == nullptr || Off > App->size || App->size - Off < Bootloader::PAYLOAD_SIZE)
            return false;
        esp_partition_read(App, Off, Data, Bootloader::PAYLOAD_SIZE);
        return true;
    };
    auto Send = [](const uint8_t *Data, size_t Length) { SendFrame(Data, Length); };
    BootloaderHost::HandleRead(Offset, ReadPayload, Send);
}

// The identify response is entirely build-time constant (app size + name), so the whole frame -
// control/parity included - is precomputed and lives in flash.
struct IdentifyFrame
{
    uint8_t Bytes[Bootloader::DATA_SIZE];
};

constexpr IdentifyFrame MakeIdentify(uint32_t Space, const char *Name)
{
    IdentifyFrame Frame{};
    Frame.Bytes[0] = Bootloader::START;
    Frame.Bytes[1] = Bootloader::CMD_IDENTIFY_RESP;
    Frame.Bytes[2] = (uint8_t)(Space & 0xFFu);
    Frame.Bytes[3] = (uint8_t)((Space >> 8) & 0xFFu);
    Frame.Bytes[4] = (uint8_t)((Space >> 16) & 0xFFu);
    Frame.Bytes[5] = (uint8_t)((Space >> 24) & 0xFFu);
    for (uint8_t i = 0; i < Bootloader::NAME_SIZE; i++)
    {
        if (Name[i] == '\0')
            break;
        Frame.Bytes[6 + i] = (uint8_t)Name[i];
    }
    Frame.Bytes[Bootloader::DATA_SIZE - 1] = Bootloader::END;
    Frame.Bytes[1] |= (uint8_t)(Bootloader::Parity(Frame.Bytes, Bootloader::DATA_SIZE) << 3);
    return Frame;
}

constexpr IdentifyFrame Identify = MakeIdentify(OTA_APP_SIZE, "Tamu v2.0A");

// One byte stream, both transports: a frame that arrives over BLE is assembled exactly the way a
// USB frame is, so the handlers cannot disagree about what a frame is.
static uint8_t FrameBuf[Bootloader::MAX_FRAME_SIZE];
static uint16_t FrameLen = 0;

static void Dispatch()
{
    const uint8_t Cmd = (uint8_t)(FrameBuf[1] & 0x07u);
    const uint32_t Offset = Bootloader::Offset(FrameBuf);
    if (Cmd == Bootloader::CMD_IDENTIFY_REQ)
        SendFrame(Identify.Bytes, Bootloader::DATA_SIZE);
    else if (Cmd == Bootloader::CMD_WRITE)
        HandleWrite(Offset, Bootloader::Payload(FrameBuf));
    else if (Cmd == Bootloader::CMD_READ_REQ)
        HandleRead(Offset);
}

static void FeedByte(uint8_t Byte)
{
    if (FrameLen == 0 && Byte != Bootloader::START)
        return; // resync on anything that is not a frame start
    FrameBuf[FrameLen++] = Byte;
    if (FrameLen < 2)
        return;

    const uint16_t Need = Bootloader::FrameSize((uint8_t)(FrameBuf[1] & 0x07u));
    if (Need == 0 || Need > sizeof(FrameBuf))
    {
        FrameLen = 0;
        return;
    }
    if (FrameLen < Need)
        return;
    if (Bootloader::Decode(FrameBuf, Need) != Bootloader::INVALID)
        Dispatch();
    FrameLen = 0;
}

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

    // NVS carries the boot budget, and the BLE controller uses it for PHY calibration data.
    esp_err_t Nvs = nvs_flash_init();
    if (Nvs == ESP_ERR_NVS_NO_FREE_PAGES || Nvs == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    const esp_partition_t *MainApp = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const bool AppImage = ImagePresent(MainApp);
    // An image the bootloader has rolled back stays unconfirmed in otadata; treat it exactly like
    // a missing one so this image parks in update mode instead of re-arming the crash for ever.
    const bool AppRolledBack = AppImage && ImageRolledBack(MainApp);

    // Let the pull-up settle, then sample: released (high) boots the main app.
    vTaskDelay(pdMS_TO_TICKS(2));
    bool ButtonHeld = gpio_get_level(BUTTON_PIN) == 0;
#ifdef TAMU_BOOTLOADER_FORCE
    // Test hook: behave as if the button is held, so the update path can be exercised without
    // physically holding GPIO2.
    ButtonHeld = true;
#endif

    FailedBoots = UpdaterBoot::LoadFailedBoots();
    ESP_LOGI(TAG, "decide: button=%d image=%d rolled_back=%d attempts=%u",
             (int)ButtonHeld, (int)AppImage, (int)AppRolledBack, (unsigned)FailedBoots);
    const UpdaterBoot::Action Action = UpdaterBoot::Decide(
        UpdaterBoot::Inputs{ButtonHeld, AppImage, AppRolledBack, FailedBoots});

    if (Action == UpdaterBoot::Action::BootApp)
    {
        UpdaterBoot::StoreFailedBoots(UpdaterBoot::NextFailedBoots(Action, FailedBoots));
        // State NEW against a rollback-enabled bootloader: the app has to prove itself (its
        // otadata erase) or the next reset rolls the slot back to this image.
        esp_err_t ArmErr = esp_ota_set_boot_partition(MainApp);
        if (ArmErr == ESP_OK)
            esp_restart();
        // The image did not validate (a partial or corrupt write). Stay here: restarting would
        // only let the bootloader fall back to this image again, and the next write fixes it.
        ESP_LOGE(TAG, "ota_0 did not validate (%d): staying in update mode", (int)ArmErr);
    }

    // Update mode. Boot signal: the notification LED is lit for as long as update mode runs. The
    // pin is active low, and while driven it can no longer be read as a button - which is fine
    // once update mode has been chosen.
    gpio_config_t Led = {};
    Led.pin_bit_mask = 1ULL << LED_PIN;
    Led.mode = GPIO_MODE_OUTPUT;
    Led.pull_up_en = GPIO_PULLUP_DISABLE;
    Led.pull_down_en = GPIO_PULLDOWN_DISABLE;
    Led.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&Led);
    gpio_set_level(LED_PIN, 0);

    usb_serial_jtag_driver_config_t Usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&Usb);
    App = MainApp;
    // Guard the sector bitmap against a `partitions.csv` size change: a larger `ota_0` would make
    // the sector index run past `Erased`. Refuse writes (reads return 0xFF) rather than corrupt
    // memory.
    if (App != nullptr && App->size > OTA_APP_SIZE)
        App = nullptr;

    WindowEndMs = UpdaterAdmission::WindowEnd(NowMs());
    BleStart();
    ESP_LOGI(TAG, "update mode: LED on, updater on USB and (window %u s) BLE",
             (unsigned)(UpdaterAdmission::WINDOW_MS / 1000));

    for (;;)
    {
        // Block for a moment instead of polling: at CONFIG_FREERTOS_HZ=100 a 1 ms delay is zero
        // ticks, so the old `vTaskDelay(pdMS_TO_TICKS(1))` spun at full speed and starved the idle
        // task into a task-watchdog report every 5 s. A 64-byte read also keeps a bulk upload off
        // the one-byte-per-pass path it would otherwise take.
        uint8_t Buf[64];
        const int Read = usb_serial_jtag_read_bytes(Buf, sizeof(Buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < Read; i++)
            FeedByte(Buf[i]);

        uint8_t Byte;
        while (RxPop(&Byte))
            FeedByte(Byte);

        if (!WindowIsOpen() && ble_gap_adv_active())
            ble_gap_adv_stop();
    }
}
