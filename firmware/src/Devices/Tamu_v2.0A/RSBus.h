#include "driver/uart.h"
#include "soc/uart_struct.h" // UART1 register struct (rxfifo_full_thrhd)
#include <string.h>
#include "Core/Functions/Bus.h"

#define TXD_PIN (GPIO_NUM_21) // RS-485 transceiver TX (to DI)
#define RXD_PIN (GPIO_NUM_20) // RS-485 transceiver RX (from RO)
#define RS485_EN_PIN (GPIO_NUM_9)

// ---------------------------------------------------------------------------------------------
// Transmit stint (cards t_4c9c5fa8 / t_45490cbe)
//
// The bound and the waits it is made of live in Core/Functions/Bus.h, which is where the arithmetic
// can be measured on the host (test/native/tx_stint_test.cpp): ONE deadline for the whole send
// (RS485_TX_STINT_MS), echo/tx-done windows sized from the line (RS485_ECHO_TIMEOUT_MS,
// RS485_TX_DONE_TIMEOUT_MS), and a link tick inside every wait (RS485AppLinkTick below).
//
// Worst case on the device: RS485_TX_STINT_MS + the last attempt's non-wait work = 373 ms. The
// longest stretch the app link goes unserved inside that is one attempt's two echo windows plus the
// first tx-done poll (22 ms, back to back: ticking between the echoes would put a gap inside the
// frame).
// The measured maximum is on the device as RS485TxStintWorstMs (BUSGATE totals line, txstint=).

// Configures UART1 as the RS-485 transceiver (TX/RX pins, baud rate, driver install, enable pin).
void SetupRS485()
{
    uart_config_t UartConfig = {
        .baud_rate = 460800,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT, // Explicitly set the clock source
        .flags = {},
    };

    // Check for errors in configuration
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &UartConfig));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Check if the driver is already installed before trying to install it again
    if (!uart_is_driver_installed(UART_NUM_1))
    {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0));
    }

    // The RX ISR only fired as the 128-byte hardware FIFO neared full, so a few ms of the
    // WS2812 LED bit-bang (interrupts masked, see LED.h) could overflow it and drop large
    // RS-485 frames. Trigger the ISR at a small FIFO level so it drains continuously and
    // the FIFO can never fill during an LED chunk.
    UART1.conf1.rxfifo_full_thrhd = 4;

    gpio_reset_pin(RS485_EN_PIN); // Ensure the pin is reset before setting direction
    gpio_set_direction(RS485_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RS485_EN_PIN, 0);
}

// ---------------------------------------------------------------------------------------------
// Received bytes that arrive while a transmit is waiting for the line (card t_4c9c5fa8)
//
// The silence wait has to read the UART - that is how it knows whether bytes are still coming -
// and the old version threw every byte it read into a 32-byte scratch array. That is how a node's
// reply died: the core decided to send while the answer was still on the wire, drained the answer
// while waiting for silence, and then reported the node as silent. Nothing counted it.
//
// Now the bytes go into a keep ring that ReceivePacket()'s byte source drains FIRST (its bytes are
// older than anything the driver still holds, so frame order is preserved). A whole longest frame
// is 129 bytes with its sync byte; 160 holds that plus the head of the next.
#define RS485_RX_KEEP_BYTES 160
// A whole longest frame (12 header + 116 payload + the 0xAA sync byte = 129) must fit, or the ring
// could refuse the very reply it exists to keep.
static_assert(RS485_RX_KEEP_BYTES >= 1 + 12 + MAX_PAYLOAD_SIZE,
              "the keep ring must hold one whole frame");
static BusRxKeepRing<RS485_RX_KEEP_BYTES> RxKeep;

// Longest single bus send since boot, ms (BUSGATE totals line prints it as txstint=).
uint32_t RS485TxStintWorstMs = 0;

// Keeps the app link alive while a send blocks the main task: the silence wait, the echo windows,
// the tx-done wait and the retry backoff can together hold it for hundreds of ms, and the app is
// mid-transaction. This is the link-only half of AppInterface.Pump() - it moves queued reply bytes
// and ticks the radios, and deliberately dispatches NOTHING: a dispatch can start another bus
// transmit, and the send that is running owns the wire.
//
// Rate-limited to ~5 ms, which is about one main-loop pass, so a stall costs the link no more
// passes than normal running does (100 ticks in a 100 ms silence window would be 10x that).
static void RS485AppLinkTick()
{
#ifdef USE_APP_INTERFACE
    static int64_t LastUs = 0;
    const int64_t Now = esp_timer_get_time();
    if (LastUs != 0 && (Now - LastUs) < 5000)
        return;
    LastUs = Now;
    AppInterface.PumpLink();
#endif
}

// Takes every byte the driver holds right now, keeps it for the assembler, and returns true when
// the line had activity (which restarts the silence window). A byte the keep ring cannot hold is
// read anyway - leaving it would corrupt the echo check below - and is counted in BusRxDestroyed.
static bool RS485KeepBufferedRx()
{
    uint8_t Buf[32];
    bool Active = false;
    for (;;)
    {
        const int Got = uart_read_bytes(UART_NUM_1, Buf, sizeof(Buf), 0);
        if (Got <= 0)
            return Active;
        Active = true;
        for (int i = 0; i < Got; i++)
        {
            if (!RxKeep.Push(Buf[i]))
            {
#ifdef TYPE_CORE
                BusRxDestroyed = BusRxDestroyed + 1; // the only way this path loses a byte
#endif
            }
        }
    }
}

// Shared CSMA/CD silence wait (Core/Functions/Bus.h): it OBSERVES the line through
// RS485KeepBufferedRx, counts silence in esp_timer microseconds and sleeps 1 ms between polls (the
// Tamu has a scheduler, and that sleep ticks the app link). Because a busy line restarts the
// window, a reply that is still arriving makes the transmit WAIT for it - the reply ends, the line
// goes quiet, and only then does the frame go out, with the reply kept for the dispatcher.
static void RS485WaitForSilence(uint8_t Priority, uint32_t CapMs)
{
    RS485WaitForSilence(Priority,
        RS485KeepBufferedRx,
        [](void) -> uint32_t { return (uint32_t)esp_timer_get_time(); },
        [](void) { RS485AppLinkTick(); vTaskDelay(pdMS_TO_TICKS(1)); },
        CapMs);
}

// Waits for the last bit to leave the wire, serving the app link between polls: uart_wait_tx_done()
// blocks for its whole timeout, so the poll is one RS485_TX_DONE_SLICE_MS slice and the tick runs
// after each one (Bus.h RS485WaitTicking).
static bool RS485WaitTxDone(uint32_t TimeoutMs)
{
    return RS485WaitTicking(
        [](void) -> bool {
            return uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(RS485_TX_DONE_SLICE_MS)) == ESP_OK;
        },
        [](void) -> uint32_t { return (uint32_t)esp_timer_get_time(); },
        [](void) { RS485AppLinkTick(); },
        [](uint32_t Ms) { vTaskDelay(pdMS_TO_TICKS(Ms)); },
        TimeoutMs, 0);
}

// The retry backoff, in RS485_BACKOFF_SLICE_MS slices so the app link does not stop for it either
// (Bus.h RS485SleepTicking).
static void RS485BackoffSleep(uint32_t Ms)
{
    RS485SleepTicking(
        [](void) -> uint32_t { return (uint32_t)esp_timer_get_time(); },
        [](void) { RS485AppLinkTick(); },
        [](uint32_t SliceMs) { vTaskDelay(pdMS_TO_TICKS(SliceMs)); },
        Ms);
}

// Sends `Data` over the RS-485 bus with CSMA/CD collision avoidance. Per Docs/RSBus and Packets.md the
// sent data is verified WHILE sending: frames go out in small chunks and each chunk's echo
// is compared as it returns, so a collision aborts mid-frame (only the queued remainder,
// <= one chunk, still leaves the wire) instead of after the whole frame. RS485_RETRIES attempts
// with random backoff, the whole stint bounded by RS485_TX_STINT_MS.
//
// The bus's ONE transmit primitive - SendAndVerifyPacket below wraps it with the transaction gate.
static bool RS485TransmitVerified(const PacketFrame &Data)
{
    // 1. A working copy, deliberately LOCAL: it frees the caller's frame as soon as we return,
    // and it lets the CRC be finalised here (the input is const). It must not reuse the shared
    // TxFrame, whose name it used to shadow.
    PacketFrame Work = Data;

    // 2. Finalize CRC (Calculated over all fields starting at flags)
    uint16_t CrcLen = 11 + PayloadBytes(Work);
    Work.Crc8 = Crc8(&Work.Flags, CrcLen);

    // 3. Prepare for transmission: the frame is header + payload bytes, max 128 total.
    // The 0xAA sync byte is sent separately (not part of the frame).
    size_t PacketSize = 12 + PayloadBytes(Work);
    static_assert(MAX_PAYLOAD_SIZE <= 116, "RSBus TX buffer assumes a 116-byte Payload");
    uint8_t TxBuffer[128];
    memcpy(TxBuffer, &Work, PacketSize);

    const size_t CHUNK = 16;               // ~1.4 ms of line time per chunk
    uint8_t RxChunk[CHUNK];

    const int64_t StintStart = esp_timer_get_time();
    for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++) {
        // One deadline for the whole send: a stuck or continuously busy bus costs this task the
        // stint, not three full attempts on top of it.
        const int64_t SpentMs = (esp_timer_get_time() - StintStart) / 1000;
        if (SpentMs >= RS485_TX_STINT_MS)
            break;
        const uint32_t LeftMs = (uint32_t)(RS485_TX_STINT_MS - SpentMs);

        // CSMA/CD: only transmit once the line has been silent. The wait KEEPS what it reads
        // (RxKeep), so a reply still arriving is waited out and handed to the assembler instead
        // of being drained into the bin.
        RS485WaitForSilence(Data.Priority,
                           LeftMs < RS485_SILENCE_TIMEOUT_MS ? LeftMs : RS485_SILENCE_TIMEOUT_MS);

        // Anything still in the driver ring arrived in the last moments of the window. It is MOVED
        // into the keep ring, never flushed: flushing here is what removed the tail of a reply
        // whose head the wait had already drained, and it was the second half of the bug. Moving
        // it also gives the echo check below an empty driver ring, which is the only thing the
        // flush was ever needed for.
        (void)RS485KeepBufferedRx();

        // Prepare bus for transmission
        gpio_set_level(RS485_EN_PIN, 1);

        bool Collided = false;
        size_t Verified = 0;

        // Sync byte first; its echo is verified too.
        uart_write_bytes(UART_NUM_1, "\xAA", 1);
        {
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, 1, pdMS_TO_TICKS(RS485_ECHO_TIMEOUT_MS));
            if (Got != 1 || RxChunk[0] != 0xAA)
                Collided = true;
        }

        for (size_t Off = 0; !Collided && Off < PacketSize; Off += CHUNK)
        {
            size_t ChunkLen = (PacketSize - Off < CHUNK) ? PacketSize - Off : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(TxBuffer + Off), ChunkLen);

            // The echo of this chunk must come back intact while we keep sending.
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, ChunkLen, pdMS_TO_TICKS(RS485_ECHO_TIMEOUT_MS));
            if (Got != (int)ChunkLen || memcmp(TxBuffer + Off, RxChunk, ChunkLen) != 0)
            {
                Collided = true;
                break;
            }
            Verified += ChunkLen;
        }

        (void)RS485WaitTxDone(RS485_TX_DONE_TIMEOUT_MS);

        // Switch back to RX mode
        gpio_set_level(RS485_EN_PIN, 0);

        if (!Collided && Verified == PacketSize)
        {
            // NOTE: no ESP_LOG here - the log goes to the USB console, and in USB APP
            // mode that byte stream belongs to the attached app (text would corrupt it).
            const uint32_t StintMs = (uint32_t)((esp_timer_get_time() - StintStart) / 1000);
            if (StintMs > RS485TxStintWorstMs)
                RS485TxStintWorstMs = StintMs;
            return true;
        }

        // Collision or failed echo: back off. The delay is 2..33 ms - at least 0.7 frame - and is
        // the address PHASE XORed with a PRNG/attempt draw, so two nodes that are still in phase
        // separate by at least 1 ms on the first retry (the old `(RawRand() % 16) + 1` was under a
        // frame at the low end, its low 4 bits cycle with period 16, and the first replacement's
        // `(Addr * 5) & 7` group made every pair of addresses 8 apart draw the same delay).
        // Derivation and the kernel analysis: Core/Functions/Bus.h RS485BackoffMs.
        const uint32_t BackoffMs =
            RS485BackoffMs(DeviceStatus.ShortAddress, (uint8_t)Attempt, RawRand());
        if (!AppInterface.Connected) // diagnostics only when no app is attached to the console
        {
            ESP_LOGW("RS485", "Attempt %d failed (%s), backoff %lums", Attempt + 1,
                     Collided ? "collision" : "echo incomplete", (unsigned long)BackoffMs);
        }
        RS485BackoffSleep(BackoffMs);
    }

    const uint32_t StintMs = (uint32_t)((esp_timer_get_time() - StintStart) / 1000);
    if (StintMs > RS485TxStintWorstMs)
        RS485TxStintWorstMs = StintMs;
    if (!AppInterface.Connected)
    {
        ESP_LOGE("RS485", "Transmission failed after %d attempts", RS485_RETRIES);
    }
#ifdef TYPE_CORE
    // Every transmit that never got through, ours or the app's: the gate ring reports the app's
    // (AppInterface.h), and a core-origin frame's failure has no other trace at all.
    AppBusSendFails = AppBusSendFails + 1;
#endif
    return false;
}

// Puts one frame on the bus, under the transaction gate.
//
// The gate is asked HERE because this is the single door every outbound frame goes through: an
// app forward (AppInterface.Pump -> DispatchPacket -> here), a core-origin request (a subscription
// install/renewal, a script's foreign register access, a confirm-required value update - these
// used to call the transmit primitive directly, so nothing gated them), a reply the core owes a
// node, and the deferred frames themselves. With the gate at the door, an answered unicast takes
// the medium's one transaction slot (AppInterface.h AppBusGateOffer / BusGate.h) and everything
// else waits for that answer instead of colliding with it.
//
// A HELD frame reports true: it was accepted, not lost - AppBusDeferredDrain puts it on the wire
// as soon as the answer it was waiting behind lands.
bool SendAndVerifyPacket(const PacketFrame &Data)
{
#ifdef USE_APP_INTERFACE
    if (!AppBusGateOffer(Data))
        return true;
#endif
    if (RS485TransmitVerified(Data))
    {
#ifdef USE_APP_INTERFACE
        // The answer is owed from the moment the frame is out, not from before the CSMA wait.
        AppBusForwardSent();
#endif
        return true;
    }
#ifdef USE_APP_INTERFACE
    // Nothing went out, so the slot this frame opened is not owed an answer: release it now rather
    // than making the queue wait out the window for a frame that never left.
    AppBusForwardFinish();
#endif
    return false;
}

// Raw bootloader send (Docs/Services/Bootloader.md): the frame is NOT a PacketFrame and
// carries no 0xAA sync, so normal nodes (which wait for 0xAA) ignore it while a bootloader
// waiting for 0xCA receives it. Same CSMA/echo discipline as SendAndVerifyPacket - and the same
// rule about received bytes: they are kept, never flushed.
bool RS485SendRaw(const uint8_t *Data, size_t Len)
{
    const size_t CHUNK = 16; // ~1.4 ms of line time per chunk
    uint8_t RxChunk[CHUNK];

    for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++) {
        RS485WaitForSilence(0, RS485_SILENCE_TIMEOUT_MS); // bootloader traffic is the highest priority
        (void)RS485KeepBufferedRx();
        gpio_set_level(RS485_EN_PIN, 1);

        bool Collided = false;
        size_t Verified = 0;
        for (size_t Off = 0; !Collided && Off < Len; Off += CHUNK) {
            size_t ChunkLen = (Len - Off < CHUNK) ? (Len - Off) : CHUNK;
            uart_write_bytes(UART_NUM_1, (const char *)(Data + Off), ChunkLen);
            int Got = uart_read_bytes(UART_NUM_1, RxChunk, ChunkLen, pdMS_TO_TICKS(RS485_ECHO_TIMEOUT_MS));
            if (Got != (int)ChunkLen || memcmp(Data + Off, RxChunk, ChunkLen) != 0) {
                Collided = true;
                break;
            }
            Verified += ChunkLen;
        }
        // Release TX-enable as soon as the last bit is out. uart_wait_tx_done() wakes on a
        // FreeRTOS notification (~a tick of latency), and the bootloader replies fast enough
        // to collide with the line while we would still be driving it; poll the UART status
        // directly so the standard silence window is enough.
        while (UART1.status.txfifo_cnt != 0 || UART1.fsm_status.st_utx_out != 0) {}
        gpio_set_level(RS485_EN_PIN, 0);

        if (!Collided && Verified == Len)
            return true;
        // Same frame-or-longer, address-decorrelated backoff as the packet path (Bus.h).
        RS485BackoffSleep(RS485BackoffMs(DeviceStatus.ShortAddress, (uint8_t)Attempt, RawRand()));
    }
    return false;
}

// While a Device bootloader passthrough is outstanding the Device service installs a sink here:
// the shared reader offers every byte to it first, so a raw 0xCA reply is stored for the
// passthrough instead of being discarded as non-0xAA noise by the packet assembler. One pass
// over the UART therefore serves both packet types. Null when nothing is expected, so the
// normal path is byte-for-byte unchanged.
bool (*BootloaderRxSink)(uint8_t Byte) = nullptr;

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the ESP32 UART driver.
//
// Bytes the transmit path took off the UART while it was waiting for the line come FIRST
// (RxKeep): they were received before anything the driver still holds, and they are a whole or
// partial frame that used to be discarded (card t_4c9c5fa8).
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &Byte) -> bool {
        for (;;) {
            if (!RxKeep.Pop(Byte) && uart_read_bytes(UART_NUM_1, &Byte, 1, 0) != 1)
                return false;
            if (BootloaderRxSink && BootloaderRxSink(Byte))
                continue; // consumed by the bootloader passthrough
            return true;
        }
    });
}
