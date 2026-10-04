"""Tamu_v2_0A upload offset: the main app lives in `ota_0`, not the factory slot.

PlatformIO's espidf builder sets `ESP32_APP_OFFSET` from the boot-default partition (`factory`,
0x10000) - which is the core bootloader. Override it so `pio run -e Tamu_v2_0A -t upload`
writes the main app to `ota_0` (0x70000) and never clobbers the factory bootloader.
"""
Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

# ota_0 in partitions.csv (factory is the 0x60000 bootloader before it).
env.Replace(ESP32_APP_OFFSET=0x70000)
