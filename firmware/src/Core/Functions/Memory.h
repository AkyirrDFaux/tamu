#pragma once

// Shared memory subsystem. Holds the common block model (BlockIndex, ValueInfo), the
// runtime block descriptors/registries used by the Dynamic and Keyed Memory services, and
// the response helpers shared by every memory service. The per-service request handling
// lives in Core/Services/<Service>.h (one file per service).
//
// Split into parts, included here in dependency order so the subsystem stays one
// translation unit (the parts share file-local statics and the block tables):
//   MemoryBackup.h   backup-file name/read/write helpers, BlockIndex, the size defaults
//   MemoryBlocks.h   the block descriptors and the generic BlockRegistry
//   MemoryDynamic.h  the dynamic registry and its create/cleanup/save/load helpers

#include "Core/Functions/MemoryBackup.h"
#include "Core/Functions/MemoryBlocks.h"
#include "Core/Functions/MemoryDynamic.h"
