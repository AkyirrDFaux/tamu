#pragma once

#include <cstdint>
#include <cstdlib>
#include "Core/Types/Log.h"
#include "Core/Functions/Packet.h"

// Device-agnostic logging interface (implemented per device, see Devices/<device>/Log.h).
// Logs a formatted message under the given `tag`
void DeviceLog(const char *tag, const char *fmt, ...);
// Logs `len` bytes of `data` as a hex dump under the given `tag`
void DeviceLogHex(const char *tag, const uint8_t *data, uint16_t len);

// Sends a log report to the LogHandler service (broadcast, outbound only). The timestamp is
// filled from the synced time before sending.
void ReportLog(LogMessage log)
{
    log.timestamp = DeviceStatus.UptimeMs;
    PacketFrame log_pkt;
    PacketConstruct(&log_pkt, ADDR_BROADCAST,
                     MakeService(ServiceType::LogHandler, 0),
                     MakeService(ServiceType::LogHandler, 0),
                     FLAG_START | FLAG_STOP,
                     (const uint8_t *)&log, sizeof(LogMessage));
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

#define MAX_LOG_RECORDS 32

#ifdef TYPE_CORE
// RAM log database on core devices, kept on the heap per Docs/Services/Log Handler.md.
// Defined in Functions/Dispatcher.h; allocated lazily by EnsureLogStorage() on first use.
extern LogRecord *LogBuffer;
extern bool *LogUsed;

// Allocates the heap-backed log database on first use (no-op afterwards).
inline void EnsureLogStorage()
{
    if (!LogBuffer)
    {
        LogBuffer = (LogRecord *)malloc(MAX_LOG_RECORDS * sizeof(LogRecord));
        LogUsed = (bool *)calloc(MAX_LOG_RECORDS, sizeof(bool));
    }
}
#endif

