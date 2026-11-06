// SIZE PROBE - not shipped firmware (kanban t_79a194d5).
//
// Answers one question before any implementation work: how much flash does a pairing-free BLE
// updater cost in the `factory` image? The shipped factory app (Bootloader/CoreBootloader.cpp)
// serves the same raw bootloader frames over the USB Serial/JTAG port and nothing else; this
// image keeps that path, adds the NimBLE host + controller with a single transfer service, and
// nothing else from the main app (no App Interface, no blocks, no register, no security
// manager, no bonds).
//
// It is a measurement, so it is deliberately self-contained: the write/read core below is a
// copy of CoreBootloader.cpp's handlers rather than a refactor of them, which keeps the
// shipped image byte-identical to what was measured before this card (172,784 B). When the
// updater is really implemented, that duplication is what to remove first.
//
// Build: pio run -e Tamu_updater_probe   (see platformio.ini; partitions_updater.csv)
//
// Behaviour it measures, and which the design rests on:
//   - boot decision (BootDecision.h): B1 held, no image in ota_0, or a spent boot budget all
//     keep the device in update mode; otherwise the app is selected and restarted into.
//   - update mode: USB frames as before, plus a GATT write/notify pair, on the air only while
//     the update window is open (UpdaterAdmission.h).
//   - a write pass that starts at offset 0 clears the boot budget, so a freshly written image
//     gets its full attempts.

#include <cstdint>
#include <cstring>

#include "Bootloader/BootDecision.h"
#include "Bootloader/UpdaterAdmission.h"
#include "Core/Functions/Bootloader.h"
#include "Core/Functions/BootloaderHost.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
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

// The board's LED-button (Docs/Devices.md): GPIO2, pull-up, active low. Update mode lights it.
#define BUTTON_PIN GPIO_NUM_2
#define LED_PIN BUTTON_PIN

// ESP32-C3 flash sector, and the size of `ota_0` in partitions_updater.csv (the sector bitmap
// below has to cover it; a smaller table would make the index run past the array).
static constexpr uint32_t SECTOR_SIZE = 4096;
static constexpr uint32_t OTA_APP_SIZE = 0x200000u;
static constexpr uint8_t IMAGE_MAGIC = 0xE9;

static const esp_partition_t *App = nullptr;
static uint8_t Erased[OTA_APP_SIZE / SECTOR_SIZE / 8u];
static_assert(OTA_APP_SIZE % SECTOR_SIZE == 0, "ota_0 size must be a whole number of sectors");

static uint32_t LastWritten = 0xFFFFFFFFu;
static uint8_t FailedBoots = 0;

static uint32_t NowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// Boot budget (NVS, namespace `updater`)
// ---------------------------------------------------------------------------

static const char *NVS_NS = "updater";
static const char *NVS_KEY = "attempts";

static uint8_t NvsAttemptsLoad()
{
    nvs_handle_t Handle;
    uint8_t Value = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &Handle) == ESP_OK)
    {
        nvs_get_u8(Handle, NVS_KEY, &Value);
        nvs_close(Handle);
    }
    return Value;
}

static void NvsAttemptsStore(uint8_t Value)
{
    nvs_handle_t Handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &Handle) != ESP_OK)
        return;
    nvs_set_u8(Handle, NVS_KEY, Value);
    nvs_commit(Handle);
    nvs_close(Handle);
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

// Peer -> device bytes. Written by the NimBLE host task (GATT access callback), read by the
// app task's loop; one producer, one consumer, so the 16-bit indices need no lock.
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
    // USB: the same bytes the shipped bootloader sends, so a USB host sees no difference.
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

    // No pairing and no passphrase, so the window is the whole admission decision: a frame
    // that arrives after it closed is refused at the attribute.
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

    struct ble_hs_adv_fields Fields = {};
    Fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    Fields.uuids128 = &SvcUuid;
    Fields.num_uuids128 = 1;
    Fields.uuids128_is_complete = 1;
    static const char Name[] = "Tamu updater";
    Fields.name = (const uint8_t *)Name;
    Fields.name_len = (uint8_t)(sizeof(Name) - 1);
    Fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&Fields) != 0)
        return;

    struct ble_gap_adv_params Params = {};
    Params.conn_mode = BLE_GAP_CONN_MODE_UND;
    Params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(OwnAddrType, nullptr, BLE_HS_FOREVER, &Params, GapEvent, nullptr);
}

static void OnSync()
{
    if (ble_hs_util_ensure_addr(0) != 0)
        return;
    if (ble_hs_id_infer_auto(0, &OwnAddrType) != 0)
        return;
    Advertise();
}

static void OnReset(int Reason) { (void)Reason; }

static void HostTask(void *Param)
{
    (void)Param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void BleStart()
{
    if (nimble_port_init() != ESP_OK)
        return;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = OnReset;
    // No ble_store_config_init(): the updater holds no bonds, and the store config is compiled
    // out with CONFIG_BT_NIMBLE_NVS_PERSIST. The window is the only admission rule.
    if (ble_gatts_count_cfg(GattSvcs) != 0)
        return;
    if (ble_gatts_add_svcs(GattSvcs) != 0)
        return;
    nimble_port_freertos_init(HostTask);
}

// ---------------------------------------------------------------------------
// Frames (Docs/Services/Bootloader.md) - dispatched from either transport
// ---------------------------------------------------------------------------

static void HandleWrite(uint32_t Offset, const uint8_t *Payload)
{
    bool Ok = (App != nullptr);
    if (Ok && (Offset & 31u) != 0)
        Ok = false; // 32-byte aligned
    if (Ok && (Offset > App->size || App->size - Offset < Bootloader::PAYLOAD_SIZE))
        Ok = false;

    if (Ok)
    {
        if (Offset == 0)
        {
            memset(Erased, 0, sizeof(Erased));
            // A new image starts here: give it the whole boot budget.
            FailedBoots = UpdaterBoot::HealthyBoots();
            NvsAttemptsStore(FailedBoots);
        }
        const uint32_t Sector = Offset / SECTOR_SIZE;
        if ((Erased[Sector >> 3] & (uint8_t)(1u << (Sector & 7u))) == 0)
        {
            if (esp_partition_erase_range(App, Sector * SECTOR_SIZE, SECTOR_SIZE) != ESP_OK)
                Ok = false;
            else
                Erased[Sector >> 3] |= (uint8_t)(1u << (Sector & 7u));
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
        if (App == nullptr || Off > App->size || App->size - Off < Bootloader::PAYLOAD_SIZE)
            return false;
        esp_partition_read(App, Off, Data, Bootloader::PAYLOAD_SIZE);
        return true;
    };
    auto Send = [](const uint8_t *Data, size_t Length) { SendFrame(Data, Length); };
    BootloaderHost::HandleRead(Offset, ReadPayload, Send);
}

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

// One byte stream, both transports: a frame that arrives over BLE is assembled exactly the way
// a USB frame is, so the handlers cannot disagree about what a frame is.
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

    bool ImagePresent = false;
    if (MainApp != nullptr)
    {
        uint8_t Magic = 0xFF;
        if (esp_partition_read(MainApp, 0, &Magic, 1) == ESP_OK)
            ImagePresent = Magic == IMAGE_MAGIC;
    }

    // Let the pull-up settle, then sample.
    vTaskDelay(pdMS_TO_TICKS(2));
    const bool ButtonHeld = gpio_get_level(BUTTON_PIN) == 0;

    FailedBoots = NvsAttemptsLoad();
    const UpdaterBoot::Action Action =
        UpdaterBoot::Decide(UpdaterBoot::Inputs{ButtonHeld, ImagePresent, FailedBoots});

    if (Action == UpdaterBoot::Action::BootApp)
    {
        NvsAttemptsStore(UpdaterBoot::NextFailedBoots(Action, FailedBoots));
        esp_ota_set_boot_partition(MainApp);
        esp_restart();
    }

    // Update mode. Boot signal: the notification LED is lit for as long as update mode runs.
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
    if (App != nullptr && App->size > OTA_APP_SIZE)
        App = nullptr; // bitmap cannot describe a larger slot: refuse writes

    WindowEndMs = UpdaterAdmission::WindowEnd(NowMs());
    BleStart();

    for (;;)
    {
        uint8_t Byte;
        if (usb_serial_jtag_read_bytes(&Byte, 1, 0) == 1)
            FeedByte(Byte);
        while (RxPop(&Byte))
            FeedByte(Byte);

        if (!WindowIsOpen() && ble_gap_adv_active())
            ble_gap_adv_stop();

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
