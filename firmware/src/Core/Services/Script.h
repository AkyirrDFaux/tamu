#pragma once

// Script service (Docs/Services/Script.md).
//
// SCR_XXX file parsing, the loaded-script registry, Register exposure (the Scripts range
// 0x3F4-0x3F7), management commands 0x0500-0x0507, and the execution engine: preloaded
// instructions, line/block tables, the VM and its state machine. The opcode/symbol encoding
// is an internal convention shared with the app's script_instructions.dart.
//
// The whole service is ONE class (ScriptService) with a single instance (`Script`). The former
// file-scope statics are now private data members and the former free functions private member
// methods, defined in the split files included after the class so the service stays a single
// translation unit:
//   ScriptDefs.h    constants, tables, the loaded-script registry types
//   ScriptProgram.h SCR_XXX parsing, program compilation, registry/entry access
//   ScriptVm.h      symbol resolution, values, assignment
//   ScriptExpr.h    infix expression evaluation (Set)
//   ScriptExec.h    per-line instruction execution
//   ScriptRuntime.h tick, boot load, management commands

#include "Core/Functions/Packet.h"

#ifdef USE_SCRIPTS

// The class declaration needs the script structures (LoadedScript is held by value in the
// registry) and the fixed-point Number type used across the VM signatures.
#include "Core/Services/ScriptDefs.h"
#include "Core/Types/Number.h"

// Defined in the split files below; only referenced by pointer/reference in the class body.
struct ScriptScalar;
struct ExprValue;
struct ExprParser;

class ScriptService
{
public:
    void Init();                           // resets the loaded-script registry and tick
    bool Handle(const PacketFrame &frame); // management CID dispatch; true if consumed

    // Exported to the rest of the tree through temporary forwarders (see below): these entry
    // points are called from the Dispatcher, Devices/*/Main.h and the Register service.
    void HandleResponse(const PacketFrame &frame);
    void Tick(uint32_t nowMs);
    void BootLoad();
    LoadedScript *Active(uint16_t slot);
    uint8_t KeyCount(uint16_t slot, uint8_t field);
    bool GetIoPointer(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, void *&data);
    bool GetEntry(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz);
    bool SetEntry(uint16_t slot, uint8_t field, uint8_t key, const ValueInfo &m,
                  const uint8_t *val, uint16_t vlen);

private:
    // ===== Loaded-script registry (was file-scope static) =====
    LoadedScript Registry[MAX_SCRIPTS];
    // One bit per slot, set for every slot that may hold a loaded script (a superset of the
    // active slots, so hot loops can skip the empty registry entries).
    uint64_t ActiveMask[kScriptMaskWords];
    uint32_t TickCount = 0;

    // ===== ScriptProgram.h: names, registry access, parsing/compilation, entry access =====
    void FileName(uint16_t id, char out[8]);
    uint16_t FileIdFromName(const char name[8]);
    uint32_t RdU32(const uint8_t *p);
    void MaskSet(uint16_t slot, bool on);
    ValueInfo *AllocMetas(uint8_t count);
    bool BuildProgram(LoadedScript *s, const uint8_t *instr, uint32_t instrLen);
    bool Load(uint16_t fileId, uint16_t slot);
    void Unload(uint16_t slot);
    uint16_t ListFiles(uint8_t *out, uint16_t max);
    bool SetVariable(uint16_t slot, uint8_t varId, const uint8_t *val, uint16_t vlen);

    // ===== ScriptVm.h: value model =====
    uint16_t SymVal(const uint8_t *sym);
    bool IsNumericDtype(uint16_t dtype);
    int32_t LoadInt(const uint8_t *d, uint8_t sz, bool sign);
    void StoreInt(uint8_t *d, uint8_t sz, int32_t v);
    bool Resolve(LoadedScript *s, uint8_t stype, uint8_t ssub, uint16_t sval,
                 uint8_t *scratch, const uint8_t **data, uint8_t *size, uint16_t *dtype);
    bool ToScalar(const uint8_t *data, uint8_t size, uint16_t dtype, bool num, ScriptScalar &out);
    bool ResolveOperandScalar(LoadedScript *s, const uint8_t *sym, bool num, ScriptScalar &out);
    bool ResolveOperandInt(LoadedScript *s, const uint8_t *sym, int32_t &out);
    bool ResolveDest(LoadedScript *s, const uint8_t *sym, uint16_t &dtype, uint8_t *&data, uint8_t &size);
    void StoreScalar(uint16_t dtype, uint8_t *data, uint8_t size, bool num, const ScriptScalar &v);
    uint8_t AssignResolved(uint16_t dtype, uint8_t *dest, uint8_t dsize,
                           const uint8_t *src, uint8_t ssize, uint16_t stype);
    uint8_t Assign(LoadedScript *s, const uint8_t *destSym, const uint8_t *src,
                   uint8_t ssize, uint16_t stype);
    uint16_t ContainerCount(uint16_t dtype, uint8_t size, const uint8_t *data);
    bool VectorElement(const uint8_t *data, uint8_t size, uint16_t dtype, uint32_t e, Number &out);
    uint8_t ExecVectorMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                           uint8_t cat, uint8_t op, uint16_t dtype, uint8_t *dest, uint8_t dsize);

    // ===== ScriptExpr.h: infix expression evaluation =====
    bool ExprResolve(ExprParser *p, const uint8_t *sym, ExprValue &out);
    uint8_t ExprPeekOp(ExprParser *p);
    bool ExprPow(Number base, Number exp, Number &out);
    bool ExprIsBoolOp(uint8_t op);
    bool ExprApply(ExprValue &a, const ExprValue &b, uint8_t op);
    bool ExprPrimary(ExprParser *p, ExprValue &out);
    bool ExprFnDot(const ExprValue &a, const ExprValue &b, ExprValue &out);
    bool ExprFnTranspose(const ExprValue &a, ExprValue &out);
    bool ExprFnSize(const ExprValue &a, ExprValue &out);
    bool ExprFnCross(const ExprValue &a, const ExprValue &b, ExprValue &out);
    bool ExprUnary(ExprParser *p, ExprValue &out);
    bool ExprBin(ExprParser *p, ExprValue &out, uint8_t minPrec);

    // ===== ScriptExec.h: per-line instruction execution =====
    uint8_t ExecSet(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                    uint16_t dtype, uint8_t *dest, uint8_t dsize);
    bool ExprTruthy(LoadedScript *s, const uint8_t *tok, uint8_t n, bool &ok);
    uint8_t ExecTransform(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                          uint16_t dtype, uint8_t *dest, uint8_t dsize);
    uint8_t ExecMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t cat, uint8_t op);
    uint8_t ExecFlow(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op);
    uint8_t ExecTime(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op,
                     uint32_t nowMs, bool &yield);
    bool OperandBlockInfo(LoadedScript *s, const uint8_t *sym, uint32_t &bi);
    void SendRegisterRequest(LoadedScript *s, uint16_t addr, uint8_t cid,
                             const uint8_t *payload, uint16_t len);
    uint8_t ExecService(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                        uint8_t op, uint32_t nowMs, bool &yield);
    bool ContainerElement(LoadedScript *s, const uint8_t *sym, int32_t index,
                          uint8_t *scratch, const uint8_t **elem, uint8_t *elemSize, uint16_t *elemType);
    uint8_t ExecCompose(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op);
    uint8_t ExecLine(LoadedScript *s, uint32_t nowMs, bool &yield);

    // ===== ScriptRuntime.h: scheduler, boot load =====
    void Run(LoadedScript *s, uint32_t nowMs);
    void SetState(LoadedScript *s, uint8_t newState);
    uint16_t FreeSlot();
};

// One instance per binary, in Core/Services/.
ScriptService Script;

inline void ScriptService::Init()
{
    memset(ActiveMask, 0, sizeof(ActiveMask));
    TickCount = 0;
    // Reset every slot to its default-constructed state (same values as the member
    // initialisers; an untouched LoadedScript holds no allocations).
    for (uint16_t i = 0; i < MAX_SCRIPTS; i++)
        Registry[i] = LoadedScript();
}

#include "Core/Services/ScriptProgram.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExpr.h"
#include "Core/Services/ScriptExec.h"
#include "Core/Services/ScriptRuntime.h"

// ---- Temporary forwarders (deleted once every caller uses `Script.` directly) ----
// Free functions with today's names/signatures that forward to the one instance, so the
// Dispatcher (HandleScript/HandleScriptResponse), Devices/*/Main.h (ScriptsTick/
// ScriptsBootLoad) and the Register service (ScriptActive/ScriptKeyCount/ScriptGetIoPointer/
// ScriptGetEntry/ScriptSetEntry) keep compiling unchanged.
void HandleScriptResponse(const PacketFrame &frame) { Script.HandleResponse(frame); }
void ScriptsTick(uint32_t nowMs) { Script.Tick(nowMs); }
void ScriptsBootLoad() { Script.BootLoad(); }
static void HandleScript(const PacketFrame &frame) { Script.Handle(frame); }
static LoadedScript *ScriptActive(uint16_t slot) { return Script.Active(slot); }
static uint8_t ScriptKeyCount(uint16_t slot, uint8_t field) { return Script.KeyCount(slot, field); }
static bool ScriptGetIoPointer(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, void *&data)
{
    return Script.GetIoPointer(slot, field, key, m, data);
}
static bool ScriptGetEntry(uint16_t slot, uint8_t field, uint8_t key, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz)
{
    return Script.GetEntry(slot, field, key, m, vbuf, vsz);
}
static bool ScriptSetEntry(uint16_t slot, uint8_t field, uint8_t key, const ValueInfo &m,
                           const uint8_t *val, uint16_t vlen)
{
    return Script.SetEntry(slot, field, key, m, val, vlen);
}

#endif // USE_SCRIPTS
