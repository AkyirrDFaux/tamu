#pragma once

// Register service (Docs/Services/Register.md): generic access to every block's fields
// through a 32-bit BlockInfo (type|instance|field|key), plus Save/Recall and the dynamic
// block management commands.
//
// Split into parts, included here in order so the service stays one translation unit:
//   RegisterDefs.h      BlockInfo accessors, response helpers
//   RegisterEnumerate.h CID 0/1 (block types, a block's Field&Keys)
//   RegisterRead.h      CID 2
//   RegisterWrite.h     CID 3
//   RegisterPersist.h   CID 4 (Recall All) / CID 5 (Save All) and 0x10-0x13
//   RegisterDispatch.h  the shared accessors and the dispatcher

#include "Core/Services/RegisterDefs.h"
#include "Core/Services/RegisterEnumerate.h"
#include "Core/Services/RegisterRead.h"
#include "Core/Services/RegisterWrite.h"
#include "Core/Services/RegisterPersist.h"
#include "Core/Services/RegisterDispatch.h"
