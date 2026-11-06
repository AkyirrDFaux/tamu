#pragma once

// Where the boot budget lives (BootDecision.h owns the numbers and the decision; this owns the
// one place it is stored). Both images touch it: the factory image reads and spends attempts, the
// main app refunds them once it has proved it is alive - so the namespace and key cannot drift
// between the two.
//
// NVS rather than a flash sector of our own: the counter is one byte that changes a handful of
// times per power cycle, and the factory image already initialises NVS for the BLE controller's
// PHY calibration data.

#include <cstdint>

#include "nvs.h"

namespace UpdaterBoot
{
    constexpr const char *NVS_NAMESPACE = "updater";
    constexpr const char *NVS_KEY = "attempts";

    inline uint8_t LoadFailedBoots()
    {
        nvs_handle_t Handle;
        uint8_t Value = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &Handle) == ESP_OK)
        {
            nvs_get_u8(Handle, NVS_KEY, &Value);
            nvs_close(Handle);
        }
        return Value;
    }

    inline void StoreFailedBoots(uint8_t Value)
    {
        nvs_handle_t Handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &Handle) != ESP_OK)
            return;
        nvs_set_u8(Handle, NVS_KEY, Value);
        nvs_commit(Handle);
        nvs_close(Handle);
    }
}
