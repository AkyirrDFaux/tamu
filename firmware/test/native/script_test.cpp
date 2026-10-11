// Host-side test for the script file header and the Service instruction set
// (Core/Services/ScriptDefs.h), against Docs/Services/Script.md.
//
// Covers the Wave-1 file-format change - a 16-bit Properties word with the load-on-boot slot at
// bits 8-15, five uint8 counts and a padding byte, then the four uint32 lengths - and the new
// Service opcodes (the retired state op, the state verbs, the custom error throw and report
// info). The header offsets are parsed through the same helper the firmware's Load() uses, so a
// mistake there fails here too.
//
// No device dependency: built with -DUSE_SCRIPTS -DSCRIPT_HOST_TEST (see run.sh), which keeps
// ScriptDefs.h on the host-clean includes (MemoryTypes/StrideOffsets/Enums/Number).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Services/ScriptDefs.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

static void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Builds a 24-byte header exactly as Docs/Services/Script.md "File Blocks" lays it out, with a
// distinctive value in every field so a swapped offset cannot pass.
static void BuildHeader(uint8_t *Buf, uint16_t Props,
                        uint8_t In, uint8_t Out, uint8_t Var, uint8_t Con, uint8_t Fn,
                        uint32_t ConstLen, uint32_t DefLen, uint32_t InstrLen, uint32_t UiLen) {
    std::memset(Buf, 0xEE, SCRIPT_HEADER_SIZE);
    Put16(Buf + 0, Props);
    Buf[2] = In; Buf[3] = Out; Buf[4] = Var; Buf[5] = Con; Buf[6] = Fn;
    Buf[7] = 0x00; // padding
    Put32(Buf + 8, ConstLen);
    Put32(Buf + 12, DefLen);
    Put32(Buf + 16, InstrLen);
    Put32(Buf + 20, UiLen);
}

int main() {
    // --- Layout constants ---
    CHECK_EQ(SCRIPT_HEADER_SIZE, 24);
    CHECK_EQ(SCRIPT_FUNC_RESERVATION_SIZE, 16);

    // --- Header round-trip ---
    {
        uint8_t Buf[SCRIPT_HEADER_SIZE];
        BuildHeader(Buf, 0x0701u /* load-on-boot, slot 7 */, 1, 2, 3, 4, 5,
                    16, 8, 40, 12);
        ScriptHeader H;
        CHECK(ParseScriptHeader(Buf, sizeof(Buf), H));
        CHECK_EQ(H.Properties, 0x0701);
        CHECK_EQ(H.InCount, 1);
        CHECK_EQ(H.OutCount, 2);
        CHECK_EQ(H.VarCount, 3);
        CHECK_EQ(H.ConstCount, 4);
        CHECK_EQ(H.FuncCount, 5);
        CHECK_EQ(H.ConstLen, 16);
        CHECK_EQ(H.DefLen, 8);
        CHECK_EQ(H.InstrLen, 40);
        CHECK_EQ(H.UiLen, 12);

        // Block sizes derived from the header: 24 (header) + (1+2+3+4)*4 ValueInfo + blobs
        // + the function reservations (5 x 16) + instructions + UI.
        const uint32_t Entries = (uint32_t)(H.InCount + H.OutCount + H.VarCount + H.ConstCount);
        const uint32_t Need = SCRIPT_HEADER_SIZE + Entries * 4 + H.ConstLen + H.DefLen
                            + (uint32_t)H.FuncCount * SCRIPT_FUNC_RESERVATION_SIZE
                            + H.InstrLen + H.UiLen;
        CHECK_EQ(Need, 24u + 40u + 16u + 8u + 80u + 40u + 12u);
    }

    // A short buffer is refused.
    {
        uint8_t Short[SCRIPT_HEADER_SIZE - 1];
        std::memset(Short, 0, sizeof(Short));
        ScriptHeader H;
        CHECK(!ParseScriptHeader(Short, sizeof(Short), H));
    }

    // The little-endian reads themselves.
    {
        const uint8_t W[2] = { 0x34, 0x12 };
        const uint8_t D[4] = { 0x78, 0x56, 0x34, 0x12 };
        CHECK_EQ(ScriptRd16(W), 0x1234);
        CHECK_EQ(ScriptRd32(D), 0x12345678u);
    }

    // --- Properties flags and slot ---
    CHECK_EQ(SCRIPT_PROP_LOAD_ON_BOOT, 0x01);
    CHECK_EQ(SCRIPT_PROP_RUN_ON_LOAD, 0x02);
    CHECK_EQ(SCRIPT_PROP_FUNCTION_LIB, 0x04);
    CHECK_EQ(SCRIPT_PROP_SLOT_SHIFT, 8);
    // The flag bits and the slot field do not overlap.
    CHECK_EQ(SCRIPT_PROP_SLOT_MASK & (SCRIPT_PROP_LOAD_ON_BOOT | SCRIPT_PROP_RUN_ON_LOAD |
                                      SCRIPT_PROP_FUNCTION_LIB), 0u);

    {
        const uint16_t Boot5 = (uint16_t)(SCRIPT_PROP_LOAD_ON_BOOT | SCRIPT_PROP_RUN_ON_LOAD |
                                          ((uint16_t)5 << SCRIPT_PROP_SLOT_SHIFT));
        CHECK(ScriptPropLoadOnBoot(Boot5));
        CHECK(ScriptPropRunOnLoad(Boot5));
        CHECK(!ScriptPropFunctionLib(Boot5));
        CHECK_EQ(ScriptPropSlot(Boot5), 5);

        const uint16_t Lib = (uint16_t)(SCRIPT_PROP_FUNCTION_LIB | ((uint16_t)200 << SCRIPT_PROP_SLOT_SHIFT));
        CHECK(!ScriptPropLoadOnBoot(Lib));
        CHECK(!ScriptPropRunOnLoad(Lib));
        CHECK(ScriptPropFunctionLib(Lib));
        CHECK_EQ(ScriptPropSlot(Lib), 200); // the whole 0-255 range stays addressable

        CHECK_EQ(ScriptPropSlot(0), 0);
        CHECK_EQ(ScriptPropSlot((uint16_t)(0xFFu << SCRIPT_PROP_SLOT_SHIFT)), 255);
    }

    // --- Service opcode set (Docs/Services/Script.md "Service") ---
    CHECK_EQ(SCRIPT_OP_SERVICE_LOG, 0);
    CHECK_EQ(SCRIPT_OP_SERVICE_REG_READ, 1);
    CHECK_EQ(SCRIPT_OP_SERVICE_REG_WRITE, 2);
    CHECK_EQ(SCRIPT_OP_SERVICE_RESERVED, 3); // the retired state instruction
    CHECK_EQ(SCRIPT_OP_SERVICE_NOP, 4);
    CHECK_EQ(SCRIPT_OP_SERVICE_REG_READ_FOREIGN, 5);
    CHECK_EQ(SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN, 6);
    CHECK_EQ(SCRIPT_OP_SERVICE_SCRIPT_LOAD, 7);
    CHECK_EQ(SCRIPT_OP_SERVICE_SCRIPT_UNLOAD, 8);
    CHECK_EQ(SCRIPT_OP_SERVICE_START, 9);
    CHECK_EQ(SCRIPT_OP_SERVICE_STOP, 10);
    CHECK_EQ(SCRIPT_OP_SERVICE_PAUSE, 11);
    CHECK_EQ(SCRIPT_OP_SERVICE_RESUME, 12);
    CHECK_EQ(SCRIPT_OP_SERVICE_ERROR, 13);
    CHECK_EQ(SCRIPT_OP_SERVICE_REPORT_INFO, 14);

    // --- Compose pair and the reserved math-op holes (sanity) ---
    CHECK_EQ(SCRIPT_OP_COMPOSE, 0);
    CHECK_EQ(SCRIPT_OP_EXTRACT, 1);
    // Shift math ops (10/11) are dropped; the next operator keeps its number.
    CHECK_EQ(SCRIPT_MATHOP_EQ, 12);

    std::printf("== script tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
