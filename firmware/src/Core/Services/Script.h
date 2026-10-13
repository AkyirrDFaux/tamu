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
    bool Handle(const PacketFrame &Frame); // management CID dispatch; true if consumed

    // Called directly across the tree: the Dispatcher (HandleResponse), the device main loops
    // (Tick, BootLoad) and the Register service (Active, KeyCount, GetIoPointer, GetEntry,
    // SetEntry).
    void HandleResponse(const PacketFrame &Frame);
    void Tick(uint32_t NowMs);
    void BootLoad();
    LoadedScript *Active(uint16_t Slot);
    uint8_t KeyCount(uint16_t Slot, uint8_t Field);
    bool GetIoPointer(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &Meta, void *&Data);
    bool GetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, ValueInfo &Meta, uint8_t *Vbuf, uint8_t &Vsz);
    bool SetEntry(uint16_t Slot, uint8_t Field, uint8_t Key, const ValueInfo &Meta,
                  const uint8_t *Val, uint16_t Vlen);

private:
    // ===== Loaded-script registry (was file-scope static) =====
    LoadedScript Registry[MAX_SCRIPTS];
    // One bit per slot, set for every slot that may hold a loaded script (a superset of the
    // active slots, so hot loops can skip the empty registry entries).
    uint64_t ActiveMask[KScriptMaskWords];
    uint32_t TickCount = 0;

    // ===== ScriptProgram.h: names, registry access, parsing/compilation, entry access =====
    void FileName(uint16_t Id, char Out[8]);
    uint16_t FileIdFromName(const char Name[8]);
    void MaskSet(uint16_t Slot, bool On);
    ValueInfo *AllocMetas(uint8_t Count);
    bool BuildProgram(LoadedScript *Script, const uint8_t *Instr, uint32_t InstrLen);
    bool Load(uint16_t FileId, uint16_t Slot);
    // True when `Slot` holds a reservation: an active script or a load-failed one (whose slot
    // record is kept until an explicit unload clears it). Occupied slots refuse a load.
    bool Occupied(uint16_t Slot);
    // Emits the load-time error log entry (Stage 1: a load failure logs through the Log
    // service) and returns false, so every refusal in Load() logs exactly once.
    bool LoadRefused(uint32_t Code);
    void Unload(uint16_t Slot);
    bool SetVariable(uint16_t Slot, uint8_t VarId, const uint8_t *Val, uint16_t Vlen);

    // ===== ScriptVm.h: value model =====
    uint16_t SymVal(const uint8_t *Sym);
    bool IsNumericDtype(uint16_t Dtype);
    int32_t LoadInt(const uint8_t *Data, uint8_t Sz, bool Sign);
    void StoreInt(uint8_t *Data, uint8_t Sz, int32_t Val);
    int32_t DecodeScalarInt(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool &Ok);
    bool Resolve(LoadedScript *Script, uint8_t Stype, uint8_t Ssub, uint16_t Sval,
                 uint8_t *Scratch, const uint8_t **Data, uint8_t *Size, uint16_t *Dtype);
    bool ToScalar(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool Num, ScriptScalar &Out);
    bool ResolveOperandScalar(LoadedScript *Script, const uint8_t *Sym, bool Num, ScriptScalar &Out);
    bool ResolveOperandInt(LoadedScript *Script, const uint8_t *Sym, int32_t &Out);
    bool ResolveDest(LoadedScript *Script, const uint8_t *Sym, uint16_t &Dtype, uint8_t *&Data, uint8_t &Size);
    void StoreScalar(uint16_t Dtype, uint8_t *Data, uint8_t Size, bool Num, const ScriptScalar &Scalar);
    uint8_t AssignResolved(uint16_t Dtype, uint8_t *Dest, uint8_t Dsize,
                           const uint8_t *Src, uint8_t Ssize, uint16_t Stype);
    uint8_t Assign(LoadedScript *Script, const uint8_t *DestSym, const uint8_t *Src,
                   uint8_t Ssize, uint16_t Stype);
    uint16_t ContainerCount(uint16_t Dtype, uint8_t Size, const uint8_t *Data);
    bool VectorElement(const uint8_t *Data, uint8_t Size, uint16_t Dtype, uint32_t Index, Number &Out);
    uint8_t ExecVectorMath(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                           uint8_t Cat, uint8_t Op, uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);

    // ===== ScriptExpr.h: infix expression evaluation =====
    bool ExprResolve(ExprParser *Parser, const uint8_t *Sym, ExprValue &Out);
    uint8_t ExprPeekOp(ExprParser *Parser);
    bool ExprPow(Number Base, Number Exp, Number &Out);
    bool ExprIsBoolOp(uint8_t Op);
    bool ExprApply(ExprValue &Lhs, const ExprValue &Rhs, uint8_t Op);
    bool ExprPrimary(ExprParser *Parser, ExprValue &Out);
    bool ExprFnDot(const ExprValue &Lhs, const ExprValue &Rhs, ExprValue &Out);
    bool ExprFnTranspose(const ExprValue &Lhs, ExprValue &Out);
    bool ExprFnSize(const ExprValue &Lhs, ExprValue &Out);
    bool ExprFnCross(const ExprValue &Lhs, const ExprValue &Rhs, ExprValue &Out);
    bool ExprUnary(ExprParser *Parser, ExprValue &Out);
    bool ExprBin(ExprParser *Parser, ExprValue &Out, uint8_t MinPrec);

    // ===== ScriptExec.h: per-line instruction execution =====
    uint8_t ExecSet(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                    uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);
    bool ExprTruthy(LoadedScript *Script, const uint8_t *Tok, uint8_t Count, bool &Ok);
    uint8_t ExecTransform(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                          uint16_t Dtype, uint8_t *Dest, uint8_t Dsize);
    uint8_t ExecMath(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Cat, uint8_t Op);
    uint8_t ExecFlow(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op);
    uint8_t ExecTime(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op,
                     uint32_t NowMs, bool &Yield);
    bool OperandBlockInfo(LoadedScript *Script, const uint8_t *Sym, uint32_t &Bi);
    void SendRegisterRequest(LoadedScript *Script, uint16_t Addr, uint8_t Cid,
                             const uint8_t *Payload, uint16_t Len);
    uint8_t ExecService(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                        uint8_t Op, uint32_t NowMs, bool &Yield);
    uint8_t StoreResultCode(LoadedScript *Script, const uint8_t *DestSym, uint8_t Code);
    bool ContainerElement(LoadedScript *Script, const uint8_t *Sym, int32_t Index,
                          uint8_t *Scratch, const uint8_t **Elem, uint8_t *ElemSize, uint16_t *ElemType);
    uint8_t ExecCompose(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase, uint8_t Op);
    uint8_t ExecLine(LoadedScript *Script, uint32_t NowMs, bool &Yield);

    // ===== ScriptRuntime.h: scheduler, boot load =====
    void Run(LoadedScript *Script, uint32_t NowMs);
    void SetState(LoadedScript *Script, uint8_t NewState);
    uint16_t FreeSlot();
    // Records a run-time fault into the CID 3 report bookkeeping and emits the error log
    // entry. `Instruction` is the offending instruction's encoding (0 = engine-level).
    void SetFault(LoadedScript *Script, uint32_t Instruction, uint32_t Err);
};

// One instance per binary, in Core/Services/.
ScriptService Script;

#include "Core/Services/ScriptProgram.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExpr.h"
#include "Core/Services/ScriptExec.h"
#include "Core/Services/ScriptRuntime.h"

#endif // USE_SCRIPTS
