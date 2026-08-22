#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>

uint32_t DeltaTime = 0;
uint32_t LastTime = 0;
int32_t TimeOffsetMs = 0;

#include "Core/Types/Number.h"
#include "Core/Types/Colour.h"
#include "Core/Types/Vector.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Enums.h"
#include "Core/Types/Log.h"

#include "Blocks/DeviceInfo.h"

// 1. Prepare the instances
DeviceStatusStruct DeviceStatus;
#ifdef BOARD_DAS_v0_1
char DeviceNameBuffer[24] = "DAS v0.1";
#else
char DeviceNameBuffer[24] = "Tamu Node";
#endif
const char* DeviceName = DeviceNameBuffer;

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
