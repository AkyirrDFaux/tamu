#pragma once

#include "Core/Services/LogHandler.h"
#include "Core/Functions/AppInterface.h"
#include "esp_log.h"
#include <cstdarg>

// Set by AppInterfaceService::Pump. While an app is attached the
// USB Serial/JTAG byte stream belongs to the app link, so free-text diagnostics must not
// be written into it (the same reason RSBus.h guards its ESP_LOG calls).

// Logs a printf-formatted message with the given tag.
//
// Two sinks, and the ORDER matters (Issues.md E67): the ring copy comes first and is
// unconditional, because while an app is connected it is the only copy left. `AppUSBActive()` is
// true whenever the cable is attached - not only while a host speaks the protocol - so a rig
// session used to see the boot banner and nothing after it. The ring is streamed out by the Log
// Handler service (CID 3, `GetTextLogs`), which is how runtime lines stay readable on the rig.
// The console copy is still suppressed while an app holds the port: the app owns that byte stream
// and a stray line would corrupt its frames.
void DeviceLog(const char *Tag, const char *Fmt, ...)
{
    char Buffer[128];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);
    LogHandler.TextLog(DeviceStatus.UptimeMs, Tag, Buffer);
    if (AppInterface.Connected)
        return;
    ESP_LOGI(Tag, "%s", Buffer);
}
