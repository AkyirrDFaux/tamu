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

    // --- CID 3 report record layout (Docs/Services/Script.md "Report Record") ---
    CHECK_EQ(SCRIPT_REPORT_OFF_STATE, 0);
    CHECK_EQ(SCRIPT_REPORT_OFF_DEPTH, 1);
    CHECK_EQ(SCRIPT_REPORT_OFF_LINE, 2);           // 2-byte boundary (depth pads it there)
    CHECK_EQ(SCRIPT_REPORT_OFF_INSTRUCTION, 4);    // 4-byte boundary
    CHECK_EQ(SCRIPT_REPORT_OFF_ERROR, 8);          // 4-byte boundary
    CHECK_EQ(SCRIPT_REPORT_SIZE, 4);               // state | depth | line
    CHECK_EQ(SCRIPT_REPORT_SIZE_ERR, 12);          // + instruction | error on an error

    // --- The report record's `instruction` encoding helper ---
    // 1 + ((category << 8) | opcode); valid instructions are never 0, so 0 can mean
    // "no instruction behind the fault" (engine-level).
    CHECK_EQ(ScriptInstructionEncoding(SCRIPT_CAT_MATH, SCRIPT_OP_MATH_SET), 1u);        // 0,0
    CHECK_EQ(ScriptInstructionEncoding(SCRIPT_CAT_FLOW, SCRIPT_OP_FLOW_CALL),
             1u + ((uint32_t)SCRIPT_CAT_FLOW << 8) + SCRIPT_OP_FLOW_CALL);
    CHECK_EQ(ScriptInstructionEncoding(SCRIPT_CAT_SERVICE, SCRIPT_OP_SERVICE_ERROR),
             1u + ((uint32_t)SCRIPT_CAT_SERVICE << 8) + SCRIPT_OP_SERVICE_ERROR);         // 4,13 -> 1038
    // A valid instruction is never 0 for any category/opcode in range.
    CHECK(ScriptInstructionEncoding(0, 0) != 0);
    CHECK(ScriptInstructionEncoding(SCRIPT_CAT_COMPOSE, SCRIPT_OP_EXTRACT) != 0);

    // --- CID 4 action enum: state verbs only; the Error op is NOT a management action ---
    {
        uint8_t St = 0xFF;
        CHECK(ScriptActionToState(SCRIPT_OP_SERVICE_START, St));
        CHECK_EQ(St, (uint8_t)ScriptState::Running);
        CHECK(ScriptActionToState(SCRIPT_OP_SERVICE_STOP, St));
        CHECK_EQ(St, (uint8_t)ScriptState::Stopped);
        CHECK(ScriptActionToState(SCRIPT_OP_SERVICE_PAUSE, St));
        CHECK_EQ(St, (uint8_t)ScriptState::Paused);
        CHECK(ScriptActionToState(SCRIPT_OP_SERVICE_RESUME, St));
        CHECK_EQ(St, (uint8_t)ScriptState::Running);
        // 13 (the in-script Error throw) and anything outside {9,10,11,12} are rejected.
        St = 0xEE;
        CHECK(!ScriptActionToState(SCRIPT_OP_SERVICE_ERROR, St));
        CHECK_EQ(St, 0xEE); // untouched on rejection
        CHECK(!ScriptActionToState(0, St));
        CHECK(!ScriptActionToState(3, St));  // the retired state op
        CHECK(!ScriptActionToState(13, St));
        CHECK(!ScriptActionToState(255, St));
    }

    // --- Stage 3: function-reservation encoding (16 bytes; a documented choice, see the
    //     header comment in ScriptDefs.h - the spec fixes the size and content, not the layout) ---
    {
        uint8_t R[SCRIPT_FUNC_RESERVATION_SIZE];
        std::memset(R, 0xAA, sizeof(R)); // the reserved bytes are ignored
        Put16(R + 0, 0x123); // file id
        Put16(R + 2, 64);    // memory-space offset
        Put16(R + 4, 7);     // instruction-space offset (in symbols)
        ScriptFuncReservation Fr = ParseFuncReservation(R);
        CHECK_EQ(Fr.FileId, 0x123);
        CHECK_EQ(Fr.MemOffset, 64);
        CHECK_EQ(Fr.InstrOffset, 7);
    }

    // --- Stage 3: global-call operand encoding (bit 15 marks a reservation index). A local/line
    //     target is always < SCRIPT_MAX_LINES, so bit 15 never collides ---
    {
        CHECK_EQ(SCRIPT_CALL_GLOBAL_BIT, 0x8000u);
        CHECK(!ScriptIsGlobalCall(0));
        CHECK(!ScriptIsGlobalCall(SCRIPT_MAX_LINES - 1));
        CHECK_EQ(ScriptGlobalCallOperand(0), 0x8000);
        CHECK_EQ(ScriptGlobalCallOperand(3), 0x8003);
        CHECK(ScriptIsGlobalCall(ScriptGlobalCallOperand(3)));
        CHECK_EQ(ScriptGlobalCallIndex(ScriptGlobalCallOperand(3)), 3);
        CHECK_EQ(ScriptGlobalCallIndex(ScriptGlobalCallOperand(0x7FFF)), 0x7FFF); // 15-bit index
    }

    // --- Stage 3: relocation arithmetic (a body placed at a base line/symbol) ---
    {
        const uint16_t Body0Base = 5, Body1Base = 8; // script is 5 lines; body 0 has 3 lines
        CHECK_EQ(ScriptRelocate(Body0Base, 0), 5);   // body-local line 0 -> absolute 5
        CHECK_EQ(ScriptRelocate(Body0Base, 2), 7);
        CHECK_EQ(ScriptRelocate(Body1Base, 1), 9);
        CHECK_EQ(ScriptRelocate(200, 4), 204);       // symbol bases relocate the same way
    }

    // --- Stage 4: lowest-free slot search (ScriptLowestFreeBit) ---
    {
        CHECK_EQ(ScriptLowestFreeBit(0), 0);                 // nothing occupied
        CHECK_EQ(ScriptLowestFreeBit(0x1), 1);               // slot 0 taken
        CHECK_EQ(ScriptLowestFreeBit(0x2), 0);               // slot 1 taken -> 0 is free
        CHECK_EQ(ScriptLowestFreeBit(0x7), 3);               // 0,1,2 taken
        CHECK_EQ(ScriptLowestFreeBit(~(uint64_t)0), 64);     // all 64 occupied
        CHECK_EQ(ScriptLowestFreeBit((uint64_t)1 << 63), 0);
        CHECK_EQ(ScriptLowestFreeBit(~((uint64_t)1 << 63)), 63); // only slot 63 free
    }

    // --- State enum values (Docs/Services/Script.md "State") ---
    // The two load states were appended so the pre-existing values keep their numbers.
    CHECK_EQ((int)ScriptState::Stopped, 0);
    CHECK_EQ((int)ScriptState::Running, 1);
    CHECK_EQ((int)ScriptState::Paused, 2);
    CHECK_EQ((int)ScriptState::Waiting, 3);
    CHECK_EQ((int)ScriptState::Finished, 4);
    CHECK_EQ((int)ScriptState::Error, 5);
    CHECK_EQ((int)ScriptState::ReservedNotReady, 6);
    CHECK_EQ((int)ScriptState::LoadFailed, 7);

    // --- Load-state helper: a slot is reserved when active OR load-failed
    //     (Docs/Services/Script.md "State") ---
    {
        LoadedScript S; // default: inactive, Stopped
        CHECK_EQ(S.Active, false);
        CHECK_EQ((int)S.State, (int)ScriptState::Stopped);
        CHECK(!ScriptSlotReserved(S)); // an empty slot reserves nothing

        S.Active = true;
        CHECK(ScriptSlotReserved(S)); // an active script reserves its slot

        S.Active = false;
        S.State = (uint8_t)ScriptState::LoadFailed;
        CHECK(ScriptSlotReserved(S)); // load-failed keeps its slot record until an unload

        S.Active = true;
        CHECK(ScriptSlotReserved(S)); // active and load-failed is still a reservation

        // The helper keys on active-or-load-failed, so a bare non-active state does not reserve.
        S.Active = false;
        const uint8_t NonReserved[] = { (uint8_t)ScriptState::Stopped, (uint8_t)ScriptState::Running,
                                        (uint8_t)ScriptState::Paused, (uint8_t)ScriptState::Waiting,
                                        (uint8_t)ScriptState::Finished, (uint8_t)ScriptState::Error,
                                        (uint8_t)ScriptState::ReservedNotReady };
        for (uint8_t St : NonReserved) {
            S.State = St;
            CHECK(!ScriptSlotReserved(S));
        }
    }

    // --- CID 5 fixed-size call-stack layout (Docs/Services/Script.md CID 5) ---
    // The reply is SCRIPT_MAX_CALL_DEPTH uint16 counter entries (outermost first) then the
    // variable RAM; the stack is fixed-size, so the two split with no delimiter.
    CHECK_EQ(SCRIPT_MAX_CALL_DEPTH, 8);      // the documented call-depth cap
    CHECK_EQ(SCRIPT_MAX_CALL_DEPTH * 2, 16); // bytes of counter stack in the reply
    {
        LoadedScript S;
        CHECK_EQ(sizeof(S.CallStack) / sizeof(S.CallStack[0]), (size_t)SCRIPT_MAX_CALL_DEPTH);
        CHECK_EQ((int)S.CallDepth, 0); // starts empty
    }
    // The cap fits the CID 3 report's uint8 depth byte, and a line index fits its uint16 field.
    CHECK(SCRIPT_MAX_CALL_DEPTH <= 0xFF);
    CHECK(SCRIPT_MAX_LINES <= 0xFFFF);

    // --- Instruction-encoding helper range (ScriptInstructionEncoding): distinct and never 0
    //     across the whole in-range category/opcode space ---
    {
        uint32_t Seen[6 * 256];
        uint16_t N = 0;
        bool NonZero = true, Distinct = true;
        for (uint8_t Cat = 0; Cat < 6; Cat++) {
            for (uint16_t Op = 0; Op < 256; Op++) {
                const uint32_t E = ScriptInstructionEncoding(Cat, (uint8_t)Op);
                if (E == 0) NonZero = false;
                for (uint16_t k = 0; k < N; k++) if (Seen[k] == E) Distinct = false;
                Seen[N++] = E;
            }
        }
        CHECK(NonZero);
        CHECK(Distinct);
        CHECK_EQ(N, 6 * 256);
    }

    // --- Global-call operand round-trip over representative indices ---
    {
        const uint16_t Idx[] = { 0, 1, 3, 255, 1023, 0x7FFF }; // 15-bit reservation index range
        for (uint16_t i : Idx) {
            const uint16_t Op = ScriptGlobalCallOperand(i);
            CHECK(ScriptIsGlobalCall(Op));
            CHECK_EQ(ScriptGlobalCallIndex(Op), i);
        }
        // An operand that already carries the marker masks cleanly (idempotent).
        CHECK_EQ(ScriptGlobalCallOperand(ScriptGlobalCallOperand(3)), ScriptGlobalCallOperand(3));
    }

    std::printf("== script tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
