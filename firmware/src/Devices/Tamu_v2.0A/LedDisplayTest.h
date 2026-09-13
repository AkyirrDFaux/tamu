#pragma once

// TEMP TEST: repeating LED display animation for the Vysi square on Display2/GPIO0,
// running entirely on the firmware (no app needed). The square moves +2 in X, then +2
// in Y, then rotates clockwise (+angle), repeating, while the fill colour cycles
// red -> green -> blue -> black. The transform resets to centre after each full cycle.

static const uint32_t LED_TEST_MOTION_MS = 500;     // motion step period
static const uint32_t LED_TEST_COLOUR_MOTIONS = 3;  // colour change every 3 motions
static const uint32_t LED_TEST_RESET_MOTIONS = 3;   // reset transform after each sequence
static const int32_t LED_TEST_ANGLE_STEP = (int32_t)(0.25f * 65536.0f); // ~14 deg/step

static DynamicBlockDescriptor *LedTestBlock = nullptr;
static uint32_t LedTestBlockIndex = 0;
static bool LedTestReady = false;

static int32_t LedTestTx = 0, LedTestTy = 0, LedTestAngle = 0;
static uint32_t LedTestMotion = 0;
static uint8_t LedTestColour = 0;
static uint32_t LedTestLastMotionMs = 0, LedTestLastColourMs = 0;

// Appends one keyed dict entry (BlockMeta + value + 4-byte padding) to `buf`.
static void LedTestPutEntry(uint8_t *buf, uint16_t &o, uint16_t type, uint8_t key, const uint8_t *val, uint8_t len)
{
    BlockMeta m = {(uint16_t)type, key, len};
    memcpy(buf + o, &m, sizeof(BlockMeta));
    o += 4;
    if (len && val)
    {
        memcpy(buf + o, val, len);
        o += len;
    }
    while (o % 4)
        buf[o++] = 0;
}

// Builds the render block: field 0 = geometry dict (Replace/Square/Position/Size/Fade/
// Alpha), field 1 = texture dict (Fill + red). Configures Display2 (GPIO0) at
// Brightness 20 and parks Display1.
static void InitLedDisplayTest()
{
    LedTestBlock = CreateDynamicBlock(BlockType::Dynamic, (const uint8_t *)"RENDER", 6);
    if (!LedTestBlock)
        return;
    LedTestBlockIndex = dynamic_block_registry.block_count - 1;

    // --- field 0: geometry dict ---
    uint8_t geo[72];
    uint16_t o = 0;
    uint8_t op = (uint8_t)GeometryOperation::Replace;
    uint8_t shape = (uint8_t)Geometries::Square;
    Matrix<2, 3> ident;
    ident(0, 0) = Number(1);
    ident(0, 1) = Number(0);
    ident(0, 2) = Number(0);
    ident(1, 0) = Number(0);
    ident(1, 1) = Number(1);
    ident(1, 2) = Number(0);
    Number size = N(4), fade = N(0), alpha = N(1);
    LedTestPutEntry(geo, o, (uint16_t)DataType::Enum, (uint8_t)GeometryKey::Operation, &op, 1);
    LedTestPutEntry(geo, o, (uint16_t)DataType::Enum, (uint8_t)GeometryKey::Shape, &shape, 1);
    LedTestPutEntry(geo, o, (uint16_t)DataType::Matrix, (uint8_t)GeometryKey::Position, (const uint8_t *)&ident.buffer, sizeof(Matrix<2, 3>));
    LedTestPutEntry(geo, o, (uint16_t)DataType::Number, (uint8_t)GeometryKey::Size, (const uint8_t *)&size, sizeof(Number));
    LedTestPutEntry(geo, o, (uint16_t)DataType::Number, (uint8_t)GeometryKey::Fade, (const uint8_t *)&fade, sizeof(Number));
    LedTestPutEntry(geo, o, (uint16_t)DataType::Number, (uint8_t)GeometryKey::Alpha, (const uint8_t *)&alpha, sizeof(Number));
    LedTestBlock->InsertField(0, {(uint16_t)DataType::Geometry, 0, (uint8_t)o});
    LedTestBlock->Set(0, geo, o, (uint16_t)DataType::Geometry);

    // --- field 1: texture dict ---
    uint8_t tex[16];
    o = 0;
    uint8_t texType = (uint8_t)Textures2D::Fill;
    ColourClass red(255, 0, 0, 255);
    LedTestPutEntry(tex, o, (uint16_t)DataType::Enum, (uint8_t)TextureKey::Type, &texType, 1);
    LedTestPutEntry(tex, o, (uint16_t)DataType::Colour, (uint8_t)TextureKey::Colour1, (const uint8_t *)&red, sizeof(ColourClass));
    LedTestBlock->InsertField(1, {(uint16_t)DataType::Texture, 0, (uint8_t)o});
    LedTestBlock->Set(1, tex, o, (uint16_t)DataType::Texture);

    // --- configure the displays ---
    Display2.Data.RenderBlock = LedTestBlockIndex;
    Display2.Data.Brightness = N(20);
    Display1.Data.RenderBlock = 0xFFFFFFFF; // park Display1 (no strip wired)

    LedTestReady = true;
}

// Animates the square: every LED_TEST_MOTION_MS applies the next motion (right +2, up
// +2, rotate +angle); every 3 motions the fill colour cycles R/G/B/black; the transform
// resets to centre after LED_TEST_RESET_MOTIONS motions.
static void TickLedDisplayTest()
{
    if (!LedTestReady || !LedTestBlock)
        return;
    uint32_t now = DeviceStatus.UptimeMs;

    if (now - LedTestLastMotionMs >= LED_TEST_MOTION_MS)
    {
        LedTestLastMotionMs = now;
        switch (LedTestMotion % 3)
        {
        case 0: LedTestTx += (2 << 16); break;              // move right (+X)
        case 1: LedTestTy += (2 << 16); break;              // move up (+Y)
        case 2: LedTestAngle += LED_TEST_ANGLE_STEP; break; // rotate clockwise (+angle)
        }
        LedTestMotion++;

        // Apply the current (post-motion) transform first...
        Number c = cos(Number::FromRaw(LedTestAngle));
        Number s = sin(Number::FromRaw(LedTestAngle));
        Matrix<2, 3> pos;
        pos(0, 0) = c;
        pos(0, 1) = -s;
        pos(0, 2) = Number::FromRaw(LedTestTx);
        pos(1, 0) = s;
        pos(1, 1) = c;
        pos(1, 2) = Number::FromRaw(LedTestTy);
        LedTestBlock->SetKey(0, (uint8_t)GeometryKey::Position, &pos.buffer, sizeof(Matrix<2, 3>), (uint16_t)DataType::Matrix);

        // ...then reset for the next cycle so the sequence stays on screen.
        if (LedTestMotion >= LED_TEST_RESET_MOTIONS)
        {
            LedTestTx = LedTestTy = LedTestAngle = 0;
            LedTestMotion = 0;
        }
    }

    if (now - LedTestLastColourMs >= LED_TEST_MOTION_MS * LED_TEST_COLOUR_MOTIONS)
    {
        LedTestLastColourMs = now;
        ColourClass colour(0, 0, 0, 255);
        switch (LedTestColour % 4)
        {
        case 0: colour = ColourClass(255, 0, 0, 255); break;
        case 1: colour = ColourClass(0, 255, 0, 255); break;
        case 2: colour = ColourClass(0, 0, 255, 255); break;
        default: colour = ColourClass(0, 0, 0, 255); break;
        }
        LedTestColour++;
        LedTestBlock->SetKey(1, (uint8_t)TextureKey::Colour1, &colour, sizeof(ColourClass), (uint16_t)DataType::Colour);
    }
}