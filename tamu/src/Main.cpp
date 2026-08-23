#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>

// The DAS reduces every DeviceLog to a structured LogHandler broadcast without free text
// (see Devices/DAS_v0.1/Log.h), so the diagnostic format strings/varargs are pure flash
// waste on this 16 KB part. The no-op macros must come before ANY Core include.
#if defined BOARD_DAS_v0_1
#define DEVICE_LOG_TEXTLESS 1
#define DeviceLog(...) ((void)0)
#define DeviceLogHex(...) ((void)0)
#endif

// 1. Prepare the instances
#ifdef BOARD_DAS_v0_1
char DeviceNameBuffer[24] = "DAS v0.1";
#else
char DeviceNameBuffer[24] = "Tamu Node";
#endif
const char* DeviceName = DeviceNameBuffer;

#include "Core/Types/Number.h"
#include "Core/Types/Colour.h"
#include "Core/Types/Vector.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Enums.h"
#include "Core/Types/Log.h"

#include "Blocks/DeviceInfo.h"

DeviceStatusStruct DeviceStatus;
uint32_t DeltaTime = 0;      // defined here (declared in Core/Functions/SysFunctions.h)
uint32_t LastTime = 0;
int32_t TimeOffsetMs = 0;

#include "Core/Functions/Packet.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Memory.h"

// 2. Create the board descriptor (defines static_block_registry + hardware-specific code)
#if defined BOARD_Tamu_v2_0A
#include "Devices/Tamu_v2.0A/Main.h"
#elif defined BOARD_DAS_v0_1
#include "Devices/DAS_v0.1/Main.h"
#endif

// 3. Dispatcher must come last (it includes Storage and all handlers that need full definitions)
#include "Core/Functions/Dispatcher.h"

// 15.06.2025 Started over for distributed system
