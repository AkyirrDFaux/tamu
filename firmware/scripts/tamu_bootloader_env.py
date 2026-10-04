"""Bootloader env helper: skip the IDF component manager.

The project's `src/idf_component.yml` pulls in the BLE managed component for the main app.
The core factory bootloader is a separate image that needs none of it, so disable the
component manager for this env before the IDF CMake build runs.
"""
import os

os.environ["IDF_COMPONENT_MANAGER"] = "0"
