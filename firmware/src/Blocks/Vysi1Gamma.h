#pragma once

// The LED output transfer curve (gamma) and the linearise step.
//
// Part of the LED display block; included by
// Blocks/Vysi1Display.h in dependency order.

#include <cstdint>
#include "Core/Types/Number.h"
#include "Core/Types/Colour.h"

// Output transfer curve: linear = 255 * (sRGB/255)^1.8 (GammaTable).
//
// A WS2812-style LED is LINEAR in PWM duty, while the render dictionary's colours are
// authored perceptually (sRGB-like). The curve therefore DARKENS the low end (exponent > 1);
// it used to apply 1/1.8, which lifted dark colours instead. Colours are now linearised ONCE,
// as they enter the render buffer (see Linearise) - so alpha compositing happens in linear
// light and the frame ends with just the Brightness scale. Blending encoded values and
// applying the curve afterwards quantised partial-alpha edges wrongly at low brightness.

#include "Blocks/Render.h"
#include "Core/Functions/Memory.h"
#include "Core/Types/Matrix.h"
#include "Core/Types/Vector.h"

const uint8_t GammaTable[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   2,
      2,   2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   6,
      6,   6,   7,   7,   8,   8,   8,   9,   9,  10,  10,  10,  11,  11,  12,  12,
     13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  21,
     21,  22,  22,  23,  24,  24,  25,  26,  26,  27,  28,  28,  29,  30,  30,  31,
     32,  32,  33,  34,  35,  35,  36,  37,  38,  38,  39,  40,  41,  41,  42,  43,
     44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  53,  54,  55,  56,  57,
     58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,
     74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  86,  87,  88,  89,  90,
     91,  92,  93,  95,  96,  97,  98,  99, 100, 102, 103, 104, 105, 107, 108, 109,
    110, 111, 113, 114, 115, 116, 118, 119, 120, 122, 123, 124, 126, 127, 128, 129,
    131, 132, 134, 135, 136, 138, 139, 140, 142, 143, 145, 146, 147, 149, 150, 152,
    153, 154, 156, 157, 159, 160, 162, 163, 165, 166, 168, 169, 171, 172, 174, 175,
    177, 178, 180, 181, 183, 184, 186, 188, 189, 191, 192, 194, 195, 197, 199, 200,
    202, 204, 205, 207, 208, 210, 212, 213, 215, 217, 218, 220, 222, 224, 225, 227,
    229, 230, 232, 234, 236, 237, 239, 241, 243, 244, 246, 248, 250, 251, 253, 255};

// Convert an authored (sRGB-ish) colour to the linear LED-duty domain. Colours are linearised
// as they enter the render buffer, so every alpha blend / composite below happens in linear
// light (blending encoded values and applying the curve afterwards quantises partial-alpha
// edges wrongly, especially at low brightness).
inline ColourClass Linearise(ColourClass c)
{
    return ColourClass(GammaTable[c.R], GammaTable[c.G], GammaTable[c.B], c.A);
}
