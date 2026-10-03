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
// Name (16 bytes) and, on a core, NetID (1 byte). The space is mirrored 1:1 to .SV. Name keeps
// a trailing NUL so DeviceName stays a valid C string while the exposed field stays 16 bytes.
#define SYSTEM_NAME_LEN 16
struct SystemPersistent
{
    char Name[SYSTEM_NAME_LEN + 1];
#ifdef TYPE_CORE
    uint8_t NetId;
#endif
};

#define SYSTEM_FIELD_NAME   6
#define SYSTEM_FIELD_NETID  7

// Number of system-block fields. NetID (7) is Core-only (Docs: "applies only after reboot")
// and App Active (8) only exists on boards with an app interface, so a plain node like the DAS
// exposes fields 0-6.
#ifdef TYPE_CORE
#define SYSTEM_FIELD_COUNT 9
#else
#define SYSTEM_FIELD_COUNT 7
#endif
