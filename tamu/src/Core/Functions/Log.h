#pragma once

#include <cstdint>
#include <cstdlib>
#include "Core/Types/Log.h"
#include "Core/Functions/Packet.h"

// Device-agnostic logging interface (implemented per device, see Devices/<device>/Log.h).
// Logs a formatted message under the given `tag`
// TEXTLESS builds (DAS) define DeviceLog/DeviceLogHex as no-op macros before including
// this header - their formatted call sites only waste flash where no text is transmitted.
#ifndef DEVICE_LOG_TEXTLESS
void DeviceLog(const char *tag, const char *fmt, ...);
// Logs `len` bytes of `data` as a hex dump under the given `tag`
void DeviceLogHex(const char *tag, const uint8_t *data, uint16_t len);
#endif

// Sends a log report to the LogHandler service (broadcast, outbound only). The timestamp is
// filled from the synced time before sending.
inline void ReportLog(const LogMessage &log)
{
    LogMessage message = log;
    message.timestamp = DeviceStatus.UptimeMs;
    PacketFrame log_pkt;
    PacketConstruct(&log_pkt, ADDR_BROADCAST,
                     MakeService(ServiceType::LogHandler, 0),
                     MakeService(ServiceType::LogHandler, 0),
                     FLAG_START | FLAG_STOP,
                     (const uint8_t *)&message, sizeof(LogMessage));
    DispatchPacket(log_pkt);
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
extern uint32_t *LogSeq;
extern uint32_t LogCapacity;

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
#endif

