#pragma once

// Vysi v1.0 LED display block (Docs/Modules and blocks/LED display.md).
//
// The block is split into parts, all included here in dependency order so it stays a
// single translation unit (the render pipeline uses file-local tables and the class's
// out-of-line member definitions):
//   Vysi1Gamma.h   the output transfer curve and Linearise
//   Vysi1Layout.h  block structs, the Vysi1Display class, layout/boot/field-write hooks
//   Vysi1Render.h  the render pipeline member definitions

#include "Blocks/Vysi1Gamma.h"
#include "Blocks/Vysi1Layout.h"
#include "Blocks/Vysi1Render.h"
