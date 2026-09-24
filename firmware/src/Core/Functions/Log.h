#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Core/Types/Log.h"
#include "Core/Functions/Packet.h"

// Device-agnostic logging interface (implemented per device, see Devices/<device>/Log.h).
// Logs a formatted message under the given `tag`
// TEXTLESS builds (DAS) define DeviceLog as a no-op macro before including
// this header - their formatted call sites only waste flash where no text is transmitted.
#ifndef DEVICE_LOG_TEXTLESS
void DeviceLog(const char *tag, const char *fmt, ...);
// Logs `len` bytes of `data` as a hex dump under the given `tag`
#endif

// Sends a log report to the LogHandler service (broadcast, outbound only). The timestamp is
// filled from the synced time before sending.
inline void ReportLog(const LogMessage &log)
{
    LogMessage message = log;
    message.timestamp = DeviceStatus.UptimeMs;
    PacketConstruct(&tx_frame, ADDR_BROADCAST,
                     MakeService(ServiceType::LogHandler, 0),
                     MakeService(ServiceType::LogHandler, 0),
                     FLAG_START | FLAG_STOP,
                     (const uint8_t *)&message, sizeof(LogMessage));
    DispatchPacket(tx_frame);
}

// Core RAM log database entry (Docs/Services/Log Handler.md):
// | Source Device | Count | Log Struct | = 16bit + 16bit + 64bit.
typedef struct __attribute__((packed))
{
    uint16_t device_id;
    uint16_t count;
    LogMessage msg;
} LogRecord;

// Heap-backed database sizing (Docs/Services/Log Handler.md): the database lives on the
// heap and GROWS when full; only when the heap cannot provide more room is the OLDEST
// record dropped to make space for the new one.
#define LOG_INITIAL_CAPACITY 32
#define LOG_GROW_STEP        16
#define LOG_MAX_CAPACITY     512 // hard safety cap

#ifdef TYPE_CORE
// RAM log database on core devices, kept on the heap (Docs/Services/Log Handler.md).
// Defined in Functions/Dispatcher.h; allocated lazily by EnsureLogStorage() on first use.
// LogSeq holds a monotonic sequence number per record so "oldest" is well defined
// (RAM-only bookkeeping; never transmitted - GetLogs streams plain LogRecords).
extern LogRecord *LogBuffer;
extern bool *LogUsed;
extern uint32_t *LogSeq;
extern uint32_t LogCapacity; // allocated slots
extern uint32_t LogCount;    // high-water mark of ever-used slots

// Allocates the initial database on first use (no-op afterwards).
inline void EnsureLogStorage()
{
    if (!LogBuffer)
    {
        LogBuffer = (LogRecord *)malloc(LOG_INITIAL_CAPACITY * sizeof(LogRecord));
        LogUsed = (bool *)calloc(LOG_INITIAL_CAPACITY, sizeof(bool));
        LogSeq = (uint32_t *)calloc(LOG_INITIAL_CAPACITY, sizeof(uint32_t));
        if (!LogBuffer || !LogUsed || !LogSeq)
        {
            // Half-initialised state would null-deref later; free and bail out.
            free(LogBuffer);
            free(LogUsed);
            free(LogSeq);
            LogBuffer = nullptr;
            LogUsed = nullptr;
            LogSeq = nullptr;
        }
        else
        {
            LogCapacity = LOG_INITIAL_CAPACITY;
        }
    }
}

// Grows the database to `new_capacity` slots. Each buffer is committed as soon as its own
// realloc succeeds: realloc frees the old block on success, so growing them all and only
// assigning at the end would leave dangling pointers if a later realloc failed (and freeing
// the grown buffers would free the live data too). On failure every pointer stays valid and
// the database simply remains at its previous capacity.
inline bool GrowLogStorage(uint32_t new_capacity)
{
    LogRecord *nb = (LogRecord *)realloc(LogBuffer, new_capacity * sizeof(LogRecord));
    if (!nb) return false;
    LogBuffer = nb;

    bool *nu = (bool *)realloc(LogUsed, new_capacity * sizeof(bool));
    if (!nu) return false;
    LogUsed = nu;

    uint32_t *ns = (uint32_t *)realloc(LogSeq, new_capacity * sizeof(uint32_t));
    if (!ns) return false;
    LogSeq = ns;

    memset(LogUsed + LogCapacity, 0, (new_capacity - LogCapacity) * sizeof(bool));
    LogCapacity = new_capacity;
    return true;
}
#endif

