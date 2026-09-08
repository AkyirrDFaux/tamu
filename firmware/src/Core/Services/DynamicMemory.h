#pragma once

#ifdef USE_DYNAMIC_MEMORY

#include <cstring>
#include <cstdlib>
#include "Core/Functions/Memory.h"

// Dynamic Memory block registry (runtime allocated).
DynamicRegistry dynamic_block_registry;

// Encoded Storage file name holding this service's backup registry.
static const char *DynamicBackupName()
{
    static constexpr char name[8] = {'D', 'Y', 'N', 'M', 'E', 'M', ' ', ' '};
    return name;
}

#endif // USE_DYNAMIC_MEMORY