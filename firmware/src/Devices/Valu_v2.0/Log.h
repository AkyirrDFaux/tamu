#pragma once

// Valu logging is compiled out entirely, exactly like the DAS: src/Main.cpp defines
// DEVICE_LOG_TEXTLESS for BOARD_Valu_v2_0 (before any Core include), which maps DeviceLog to
// a no-op macro - this device's only byte stream is the USB CDC App Interface, which the app
// link owns (free-text diagnostics written there would corrupt the packet stream), and the
// formatted call sites would otherwise be pure flash waste. Real, structured error reporting
// goes through ReportLog(MakeLog(...)) (LogHandler), which reaches the core over RSBus on a
// normal network. This file exists so Device.h's `#include` of the device Log header stays
// uniform with the other builds; there is deliberately nothing to define here.
#include "Core/Services/LogHandler.h"
