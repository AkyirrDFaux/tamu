#pragma once

// File-system layer (Docs/Services/Storage.md). Files are stored as 16-byte FileEntry
// records in a table page; a file's data occupies whole pages after it. Two backends are
// selected at compile time:
//   - the full multi-file system (Tamu),
//   - USE_FIXED_STORAGE: the DAS's reduced variant (Devices.md), fixed size/positions.
//
// Split into parts, included here so the layer stays one translation unit:
//   StorageDefs.h      geometry, flash hooks, FileEntry and its helpers
//   StorageBlockFS.h   the full multi-file system
//   StorageFixedFS.h   the fixed-size variant

#include "Core/Functions/StorageDefs.h"

// Full multi-file file system (Tamu + any target without USE_FIXED_STORAGE).
#ifndef USE_FIXED_STORAGE
#include "Core/Functions/StorageBlockFS.h"
#else
#include "Core/Functions/StorageFixedFS.h"
#endif // USE_FIXED_STORAGE
