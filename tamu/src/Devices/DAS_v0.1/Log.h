#pragma once

#include "Core/Services/LogHandler.h"

// When DEVICE_LOG_TEXTLESS is defined (DAS Main.h), DeviceLog/DeviceLogHex are macros that
// compile the formatted call sites out entirely; only the definitions below are skipped.
// ReportLog(MakeLog(...)) calls carry the real error codes.
#ifndef DEVICE_LOG_TEXTLESS

// Reports a Device-service log over RS485 as a structured LogHandler broadcast.
// The doc Log Struct carries only source/code/timestamp (no free text), so the formatted
// diagnostics are reduced to a generic Device-service log. Specific error codes should be
// reported directly via ReportLog(MakeLog(...)).
void DeviceLog(const char *tag, const char *fmt, ...)
{
    (void)tag; (void)fmt;
    if (!g_rs485_ready)
        return;
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0));
}

// Reports a Device-service log for a hex dump (text is not carried by the doc log format).
void DeviceLogHex(const char *tag, const uint8_t *data, uint16_t len)
{
    (void)tag; (void)data; (void)len;
    if (!g_rs485_ready)
        return;
    ReportLog(MakeLog(false, (uint16_t)ServiceType::Device, 0, 0));
}

#endif // DEVICE_LOG_TEXTLESS
