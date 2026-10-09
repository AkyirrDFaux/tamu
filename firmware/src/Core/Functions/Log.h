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

// Sends a log report to the local core's LogHandler service (Docs/Services/Log Handler.md:
// "local core (ID 0.1)"). Address 0.1 has net 0 = the local net, resolved at match time
// (`NetQualifyLocal`), so a node reaches its own net's core and a core addresses itself;
// a broadcast would instead make every core on a shared bus store every node's log. The
// timestamp is filled from the synced time before sending.
//
// `priority` is the log's class (Docs/Services/Log Handler.md: "at the priority that
// corresponds to it"): an error report goes out at PRIORITY_ERROR, an informational log at
// PRIORITY_LOG. The caller states it per case - there is no default, so every send site has
// to say which of the two it is.
inline void ReportLog(const LogMessage &log, uint8_t priority)
{
    LogMessage message = log;
    message.timestamp = DeviceStatus.UptimeMs;
    PacketConstruct(&tx_frame, MakeId(0, 1), // 0.1 = the local core
                     MakeService(ServiceType::LogHandler, 0),
                     NextSystemTrid(ServiceType::LogHandler),
                     FLAG_START | FLAG_STOP,
                     (const uint8_t *)&message, sizeof(LogMessage),
                     priority);
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

// Grows the database to `new_capacity` slots atomically: all three new blocks are allocated
// first and only swapped in once every allocation succeeded, so a partial failure leaves the
// live database exactly as it was (no half-grown buffer committed). The old blocks are freed
// after the copy.
inline bool GrowLogStorage(uint32_t new_capacity)
{
    LogRecord *nb = (LogRecord *)malloc(new_capacity * sizeof(LogRecord));
    bool *nu = (bool *)malloc(new_capacity * sizeof(bool));
    uint32_t *ns = (uint32_t *)malloc(new_capacity * sizeof(uint32_t));
    if (!nb || !nu || !ns)
    {
        free(nb);
        free(nu);
        free(ns);
        return false;
    }

    memcpy(nb, LogBuffer, LogCapacity * sizeof(LogRecord));
    memcpy(nu, LogUsed, LogCapacity * sizeof(bool));
    memcpy(ns, LogSeq, LogCapacity * sizeof(uint32_t));
    memset(nu + LogCapacity, 0, (new_capacity - LogCapacity) * sizeof(bool));
    memset(ns + LogCapacity, 0, (new_capacity - LogCapacity) * sizeof(uint32_t));

    free(LogBuffer);
    free(LogUsed);
    free(LogSeq);
    LogBuffer = nb;
    LogUsed = nu;
    LogSeq = ns;
    LogCapacity = new_capacity;
    return true;
}
#endif

