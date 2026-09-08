#pragma once

#include "Core/Services/LogHandler.h"
#include "esp_log.h"
#include <cstdarg>

// Logs a printf-formatted message with the given tag via ESP_LOGI.
void DeviceLog(const char *tag, const char *fmt, ...)
{
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ESP_LOGI(tag, "%s", buffer);
}

// Logs `len` bytes of `data` as a hex dump under the given tag.
void DeviceLogHex(const char *tag, const uint8_t *data, uint16_t len)
{
    ESP_LOG_BUFFER_HEX(tag, data, len);
}