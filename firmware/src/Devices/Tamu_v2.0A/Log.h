#pragma once

#include "Core/Services/LogHandler.h"
#include "esp_log.h"
#include <cstdarg>

// Set by AppInterfacePump (Core/Functions/AppInterface.h). While an app is attached the
// USB Serial/JTAG byte stream belongs to the app link, so free-text diagnostics must not
// be written into it (the same reason RSBus.h guards its ESP_LOG calls).
extern bool AppConnected;

// Logs a printf-formatted message with the given tag via ESP_LOGI. Suppressed while an app
// is connected (see AppConnected above).
void DeviceLog(const char *tag, const char *fmt, ...)
{
    if (AppConnected)
        return;
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ESP_LOGI(tag, "%s", buffer);
}

