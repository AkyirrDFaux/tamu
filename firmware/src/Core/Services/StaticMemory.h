#pragma once

#include "Core/Functions/Memory.h"

// The static memory's persistent half is mirrored 1:1 to a single file, `.SV`
// (Docs/Services/Register.md "Persistence"). The device only ever overwrites/loads the whole
// file; targeted saves/recalls are the app's job (direct file writes at a field's offset).
static const char *StaticValuesName()
{
    static constexpr char name[8] = {'.', 'S', 'V', ' ', ' ', ' ', ' ', ' '};
    return name;
}

// The System block (BlockType 0) is a special static block every device has. Its persistent
// fields are the first segment of the static persistent space (Docs/Services/Register.md):
// Name (16 bytes) and, on a core, NetID (1 byte). The space is mirrored 1:1 to .SV. Name is a
// fixed 16-char field, space-padded, with no terminator.
#define SYSTEM_NAME_LEN 16
struct SystemPersistent
{
    char Name[SYSTEM_NAME_LEN];
#ifdef TYPE_CORE
    uint8_t NetId;
#endif
};

#define SYSTEM_FIELD_NAME   6
#define SYSTEM_FIELD_NETID  7
