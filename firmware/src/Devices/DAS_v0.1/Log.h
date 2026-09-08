#pragma once

// DAS logging is compiled out entirely: src/Main.cpp defines DEVICE_LOG_TEXTLESS for
// BOARD_DAS_v0_1 (before any Core include), which maps DeviceLog/DeviceLogHex to no-op
// macros - the DAS log format carries no free text, so the formatted call sites are pure
// flash waste. Real error reporting goes through ReportLog(MakeLog(...)) (LogHandler).
// This file exists so Device.h's `#include` of the device Log header stays uniform with
// the Tamu build; there is deliberately nothing to define here.
#include "Core/Services/LogHandler.h"