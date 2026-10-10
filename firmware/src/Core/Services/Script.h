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
    bool Handle(const PacketFrame &Frame); // management CID dispatch; true if consumed

    // Exported to the rest of the tree through temporary forwarders (see below): these entry
    // points are called from the Dispatcher, Devices/*/Main.h and the Register service.
    void HandleResponse(const PacketFrame &Frame);
    void Tick(uint32_t nowMs);
    void BootLoad();
    LoadedScript *Active(uint16_t Slot);
    uint8_t KeyCount(uint16_t Slot, uint8_t Field);
    bool GetIoPointer(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &m, void *&Data);
    bool GetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &m, uint8_t *Vbuf, uint8_t &Vsz);
    bool SetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, const ValueInfo &m,
                  const uint8_t *Val, uint16_t Vlen);

private:
    // ===== Loaded-script registry (was file-scope static) =====
    LoadedScript Registry[MAX_SCRIPTS];
    // One bit per slot, set for every slot that may hold a loaded script (a superset of the
    // active slots, so hot loops can skip the empty registry entries).
    uint64_t ActiveMask[kScriptMaskWords];
    uint32_t TickCount = 0;

    // ===== ScriptProgram.h: names, registry access, parsing/compilation, entry access =====
    void FileName(uint16_t Id, char Out[8]);
    uint16_t FileIdFromName(const char Name[8]);
    uint32_t RdU32(const uint8_t *p);
    void MaskSet(uint16_t Slot, bool On);
    ValueInfo *AllocMetas(uint8_t Count);
    bool BuildProgram(LoadedScript *s, const uint8_t *Instr, uint32_t instrLen);
    bool Load(uint16_t fileId, uint16_t Slot);
    void Unload(uint16_t Slot);
    uint16_t ListFiles(uint8_t *Out, uint16_t Max);
    bool SetVariable(uint16_t Slot, uint8_t varId, const uint8_t *Val, uint16_t Vlen);

    // ===== ScriptVm.h: value model =====
    uint16_t SymVal(const uint8_t *Sym);
    bool IsNumericDtype(uint16_t Dtype);
    int32_t LoadInt(const uint8_t *d, uint8_t Sz, bool Sign);
    void StoreInt(uint8_t *d, uint8_t Sz, int32_t v);
    bool Resolve(LoadedScript *s, uint8_t Stype, uint8_t Ssub, uint16_t Sval,
                 uint8_t *Scratch, const uint8_t **Data, uint8_t *Size, uint16_t *Dtype);
    bool ToScalar(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool Num, ScriptScalar &Out);
    bool ResolveOperandScalar(LoadedScript *s, const uint8_t *Sym, bool Num, ScriptScalar &Out);
    bool ResolveOperandInt(LoadedScript *s, const uint8_t *Sym, int32_t &Out);
    bool ResolveDest(LoadedScript *s, const uint8_t *Sym, uint16_t &Dtype, uint8_t *&Data, uint8_t &Size);
    void StoreScalar(uint16_t Dtype, uint8_t *Data, uint8_t Size, bool Num, const ScriptScalar &v);
    uint8_t AssignResolved(uint16_t Dtype, uint8_t *Dest, uint8_t Dsize,
                           const uint8_t *Src, uint8_t Ssize, uint16_t Stype);
    uint8_t Assign(LoadedScript *s, const uint8_t *destSym, const uint8_t *Src,
                   uint8_t Ssize, uint16_t Stype);
    uint16_t ContainerCount(uint16_t Dtype, uint8_t Size, const uint8_t *Data);
    bool VectorElement(const uint8_t *Data, uint8_t Size, uint16_t Dtype, uint32_t e, Number &Out);
    uint8_t ExecVectorMath(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                           uint8_t Cat, uint8_t Op, uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);

    // ===== ScriptExpr.h: infix expression evaluation =====
    bool ExprResolve(ExprParser *p, const uint8_t *Sym, ExprValue &Out);
    uint8_t ExprPeekOp(ExprParser *p);
    bool ExprPow(Number Base, Number Exp, Number &Out);
    bool ExprIsBoolOp(uint8_t Op);
    bool ExprApply(ExprValue &a, const ExprValue &b, uint8_t Op);
    bool ExprPrimary(ExprParser *p, ExprValue &Out);
    bool ExprFnDot(const ExprValue &a, const ExprValue &b, ExprValue &Out);
    bool ExprFnTranspose(const ExprValue &a, ExprValue &Out);
    bool ExprFnSize(const ExprValue &a, ExprValue &Out);
    bool ExprFnCross(const ExprValue &a, const ExprValue &b, ExprValue &Out);
    bool ExprUnary(ExprParser *p, ExprValue &Out);
    bool ExprBin(ExprParser *p, ExprValue &Out, uint8_t minPrec);

    // ===== ScriptExec.h: per-line instruction execution =====
    uint8_t ExecSet(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                    uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);
    bool ExprTruthy(LoadedScript *s, const uint8_t *Tok, uint8_t n, bool &Ok);
    uint8_t ExecTransform(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                          uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);
    uint8_t ExecMath(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Cat, uint8_t Op);
    uint8_t ExecFlow(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op);
    uint8_t ExecTime(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op,
                     uint32_t nowMs, bool &Yield);
    bool OperandBlockInfo(LoadedScript *s, const uint8_t *Sym, uint32_t &Bi);
    void SendRegisterRequest(LoadedScript *s, uint16_t Addr, uint8_t Cid,
                             const uint8_t *Payload, uint16_t Len);
    uint8_t ExecService(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                        uint8_t Op, uint32_t nowMs, bool &Yield);
    bool ContainerElement(LoadedScript *s, const uint8_t *Sym, int32_t Index,
                          uint8_t *Scratch, const uint8_t **Elem, uint8_t *elemSize, uint16_t *elemType);
    uint8_t ExecCompose(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op);
    uint8_t ExecLine(LoadedScript *s, uint32_t nowMs, bool &Yield);

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

#endif // USE_SCRIPTS
