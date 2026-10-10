#pragma once

#include "Core/Services/LogHandler.h"
#include "Core/Functions/AppInterface.h"
#include "esp_log.h"
#include <cstdarg>

// Set by AppInterfaceService::Pump. While an app is attached the
// USB Serial/JTAG byte stream belongs to the app link, so free-text diagnostics must not
// be written into it (the same reason RSBus.h guards its ESP_LOG calls).

// Logs a printf-formatted message with the given tag via ESP_LOGI. Suppressed while an app
// is connected (see AppInterface.Connected above).
void DeviceLog(const char *Tag, const char *Fmt, ...)
{
    if (AppInterface.Connected)
        return;
    char Buffer[128];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);
    ESP_LOGI(Tag, "%s", Buffer);
}

