#pragma once

// Register service (Docs/Services/Register.md): generic access to every block's fields
// through a 32-bit BlockInfo (type|instance|field|key), plus Save/Recall and the dynamic
// block management commands.
//
// Split into parts, included here in order so the service stays one translation unit:
//   RegisterDefs.h      BlockInfo accessors, response helpers
//   RegisterEnumerate.h CID 0
//   RegisterRead.h      CID 1
//   RegisterWrite.h     CID 2
//   RegisterPersist.h   CID 3/4 and 0x10-0x15
//   RegisterDispatch.h  the shared accessors and the dispatcher

#include "Core/Services/RegisterDefs.h"
#include "Core/Services/RegisterEnumerate.h"
#include "Core/Services/RegisterRead.h"
#include "Core/Services/RegisterWrite.h"
#include "Core/Services/RegisterPersist.h"
#include "Core/Services/RegisterDispatch.h"
