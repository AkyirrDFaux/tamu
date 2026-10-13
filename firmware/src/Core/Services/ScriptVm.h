#pragma once

// VM core: scalar/vector resolution and assignment (ScriptVm.h) - part of the script service.
//
// The VM's value model: symbol resolution, the ScriptScalar/vector element
// helpers, assignment and the vector math instruction.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include <cstring>
#include "Core/Types/Number.h"

// ===== VM =====

struct ScriptScalar {
    Number Num;
    int32_t Int;
};

// SetState is declared as a ScriptService member (Script.h) and defined in ScriptRuntime.h;
// the Script-state opcode uses it so self state changes follow the same transition rules
// (IC reset, pending-confirmation clearing) as CID 4.

inline uint16_t ScriptService::SymVal(const uint8_t *Sym) { return ScriptRd16(Sym + 2); }

inline bool ScriptService::IsNumericDtype(uint16_t Dtype) {
    switch (Dtype) {
        case (uint16_t)DataType::Bool:
        case (uint16_t)DataType::Index:
        case (uint16_t)DataType::Number:
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::Uint32:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id:
            return true;
        default:
            return false;
    }
}

inline int32_t ScriptService::LoadInt(const uint8_t *Data, uint8_t Sz, bool Sign) {
    int32_t Acc = 0;
    for (uint8_t i = 0; i < Sz && i < 4; i++) Acc |= (int32_t)Data[i] << (8 * i);
    if (Sign && Sz > 0 && Sz < 4 && (Data[Sz - 1] & 0x80)) Acc |= ~((1 << (8 * Sz)) - 1);
    return Acc;
}

inline void ScriptService::StoreInt(uint8_t *Data, uint8_t Sz, int32_t Acc) {
    for (uint8_t i = 0; i < Sz && i < 4; i++) Data[i] = (uint8_t)((Acc >> (8 * i)) & 0xFF);
    for (uint8_t i = 4; i < Sz; i++) Data[i] = 0;
}

// Resolves a source symbol into bytes. Predefines are materialised into `scratch`.
inline bool ScriptService::Resolve(LoadedScript *Script, uint8_t Stype, uint8_t Ssub, uint16_t Sval,
                          uint8_t *Scratch, const uint8_t **Data, uint8_t *Size, uint16_t *Dtype) {
    switch (Stype) {
        case SCRIPT_SYM_INPUT:
            if (Sval >= Script->InCount || !Script->IoSpace) return false;
            *Data = Script->IoSpace + Script->InputOffset((uint8_t)Sval);
            *Size = Script->InMeta[Sval].Size;
            *Dtype = ValueInfoType(Script->InMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_OUTPUT:
            if (Sval >= Script->OutCount || !Script->IoSpace) return false;
            *Data = Script->IoSpace + Script->OutputOffset((uint8_t)Sval);
            *Size = Script->OutMeta[Sval].Size;
            *Dtype = ValueInfoType(Script->OutMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_VARIABLE:
            if (Sval >= Script->VarCount) return false;
            *Data = Script->VarSpace + Script->VarOffset((uint8_t)Sval);
            *Size = Script->VarMeta[Sval].Size;
            *Dtype = ValueInfoType(Script->VarMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_CONSTANT:
            if (Sval >= Script->ConstCount) return false;
            *Data = Script->ConstSpace + Script->ConstOffset((uint8_t)Sval);
            *Size = Script->ConstMeta[Sval].Size;
            *Dtype = ValueInfoType(Script->ConstMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_PREDEFINE:
            memset(Scratch, 0, 4);
            switch (Ssub) {
                case SCRIPT_PRE_INDEX: Scratch[0] = Sval & 0xFF; Scratch[1] = (Sval >> 8) & 0xFF; *Size = 2; *Dtype = (uint16_t)DataType::Index; break;
                case SCRIPT_PRE_CHAR:  Scratch[0] = Sval & 0xFF; *Size = 1; *Dtype = (uint16_t)DataType::Uint32; break;
                case SCRIPT_PRE_BOOL:  Scratch[0] = Sval ? 1 : 0; *Size = 1; *Dtype = (uint16_t)DataType::Bool; break;
                case SCRIPT_PRE_STATE: Scratch[0] = Sval & 0xFF; *Size = 1; *Dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_MATHOP:Scratch[0] = Sval & 0xFF; *Size = 1; *Dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_TYPE:  Scratch[0] = Sval & 0xFF; Scratch[1] = (Sval >> 8) & 0xFF; *Size = 2; *Dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_NUMBER: {
                    // 16-bit Q8.8 literal -> 16.16 Number (e.g. 128 -> 0.5).
                    int32_t Raw = (int32_t)(int16_t)Sval << 8;
                    memcpy(Scratch, &Raw, 4);
                    *Size = 4; *Dtype = (uint16_t)DataType::Number; break;
                }
                default: return false;
            }
            *Data = Scratch;
            return true;
        default:
            return false;
    }
}

// Decodes a scalar value's bytes to its raw integer form: the Q16.16 word for Number, the
// sign/width-correct integer for the integer dtypes. `Ok` reports whether the dtype is a
// scalar that decodes; the Q16.16/plain-integer choice is left to the caller.
inline int32_t ScriptService::DecodeScalarInt(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool &Ok) {
    Ok = true;
    switch (Dtype) {
        case (uint16_t)DataType::Number: {
            if (Size < 4) { Ok = false; return 0; }
            int32_t Raw = 0; memcpy(&Raw, Data, 4); return Raw;
        }
        case (uint16_t)DataType::Index: return LoadInt(Data, Size, true);
        case (uint16_t)DataType::Uint32: return LoadInt(Data, Size, false);
        case (uint16_t)DataType::Bool: return Data[0] ? 1 : 0;
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id: return LoadInt(Data, Size, false);
        default: Ok = false; return 0;
    }
}

inline bool ScriptService::ToScalar(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool Num, ScriptScalar &Out) {
    Out.Num = Number(0);
    Out.Int = 0;
    bool Ok = false;
    int32_t Raw = DecodeScalarInt(Data, Size, Dtype, Ok);
    if (!Ok) return false;
    const bool IsNum = (Dtype == (uint16_t)DataType::Number);
    if (Num) Out.Num = IsNum ? Number::FromRaw(Raw) : Number(Raw);
    else     Out.Int = IsNum ? Number::FromRaw(Raw).ToInt() : Raw;
    return true;
}

inline bool ScriptService::ResolveOperandScalar(LoadedScript *Script, const uint8_t *Sym, bool Num, ScriptScalar &Out) {
    uint8_t Scratch[4];
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(Script, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    return ToScalar(Data, Size, Dtype, Num, Out);
}

inline bool ScriptService::ResolveOperandInt(LoadedScript *Script, const uint8_t *Sym, int32_t &Out) {
    ScriptScalar Sc;
    if (!ResolveOperandScalar(Script, Sym, false, Sc)) return false;
    Out = Sc.Int;
    return true;
}

// Resolves a destination symbol (variable or output) into its storage.
inline bool ScriptService::ResolveDest(LoadedScript *Script, const uint8_t *Sym, uint16_t &Dtype, uint8_t *&Data, uint8_t &Size) {
    if (Sym[0] == SCRIPT_SYM_VARIABLE) {
        uint16_t Idx = SymVal(Sym);
        if (Idx >= Script->VarCount) return false;
        Data = Script->VarSpace + Script->VarOffset((uint8_t)Idx);
        Size = Script->VarMeta[Idx].Size;
        Dtype = ValueInfoType(Script->VarMeta[Idx].Type);
        return true;
    }
    if (Sym[0] == SCRIPT_SYM_OUTPUT) {
        uint16_t Idx = SymVal(Sym);
        if (Idx >= Script->OutCount || !Script->IoSpace) return false;
        Data = Script->IoSpace + Script->OutputOffset((uint8_t)Idx);
        Size = Script->OutMeta[Idx].Size;
        Dtype = ValueInfoType(Script->OutMeta[Idx].Type);
        return true;
    }
    return false;
}

inline void ScriptService::StoreScalar(uint16_t Dtype, uint8_t *Data, uint8_t Size, bool Num, const ScriptScalar &Scalar) {
    switch (Dtype) {
        case (uint16_t)DataType::Number: {
            int32_t Raw = Num ? Scalar.Num.Value : Number(Scalar.Int).Value;
            memcpy(Data, &Raw, 4);
            break;
        }
        case (uint16_t)DataType::Bool: {
            int32_t Iv = Num ? Scalar.Num.RoundToInt() : Scalar.Int;
            Data[0] = Iv ? 1 : 0;
            break;
        }
        default: {
            int32_t Iv = Num ? Scalar.Num.RoundToInt() : Scalar.Int;
            StoreInt(Data, Size, Iv);
            break;
        }
    }
}

// Stores a resolved source into an already-resolved destination (numeric conversion when
// both sides are numeric, raw clamped copy otherwise).
inline uint8_t ScriptService::AssignResolved(uint16_t Dtype, uint8_t *Dest, uint8_t Dsize,
                                    const uint8_t *Src, uint8_t Ssize, uint16_t Stype) {
    if (IsNumericDtype(Dtype) && IsNumericDtype(Stype)) {
        ScriptScalar Sc;
        bool Num = (Dtype == (uint16_t)DataType::Number);
        if (!ToScalar(Src, Ssize, Stype, Num, Sc)) return SCRIPT_ERR_TYPE;
        StoreScalar(Dtype, Dest, Dsize, Num, Sc);
    } else {
        uint8_t Len = Ssize < Dsize ? Ssize : Dsize;
        if (Len) memcpy(Dest, Src, Len);
        for (uint8_t i = Len; i < Dsize; i++) Dest[i] = 0;
    }
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::Assign(LoadedScript *Script, const uint8_t *DestSym, const uint8_t *Src,
                            uint8_t Ssize, uint16_t Stype) {
    uint16_t Dtype = 0;
    uint8_t *Dest = nullptr;
    uint8_t Dsize = 0;
    if (!ResolveDest(Script, DestSym, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
    return AssignResolved(Dtype, Dest, Dsize, Src, Ssize, Stype);
}

// Number of elements in a Vector/Matrix value (0 for scalars / malformed containers).
inline uint16_t ScriptService::ContainerCount(uint16_t Dtype, uint8_t Size, const uint8_t *Data) {
    if (Dtype == (uint16_t)DataType::Vector) return Size / 4;
    if (Dtype == (uint16_t)DataType::Matrix) {
        if (Size < 4 || !Data) return 0;
        uint16_t Rows = ScriptRd16(Data);
        uint16_t Cols = ScriptRd16(Data + 2);
        uint32_t Count = (uint32_t)Rows * Cols;
        if (Count > (uint32_t)(Size - 4) / 4) return 0;
        return (uint16_t)Count;
    }
    return 0;
}

// Reads element `e` of a value as a Number. Scalars broadcast (the same value for any e).
inline bool ScriptService::VectorElement(const uint8_t *Data, uint8_t Size, uint16_t Dtype, uint32_t Index, Number &Out) {
    if (Dtype == (uint16_t)DataType::Vector) {
        if ((Index + 1) * 4 > Size) return false;
        int32_t Raw = 0;
        memcpy(&Raw, Data + Index * 4, 4); Out = Number::FromRaw(Raw); return true;
    }
    if (Dtype == (uint16_t)DataType::Matrix) {
        if (Index >= ContainerCount(Dtype, Size, Data)) return false;
        int32_t Raw = 0;
        memcpy(&Raw, Data + 4 + Index * 4, 4); Out = Number::FromRaw(Raw); return true;
    }
    bool Ok = false;
    int32_t Raw = DecodeScalarInt(Data, Size, Dtype, Ok);
    if (!Ok) return false;
    Out = (Dtype == (uint16_t)DataType::Number) ? Number::FromRaw(Raw) : Number(Raw);
    return true;
}

// Element-wise math on a Vector/Matrix destination (Mod/Min/Max/Abs/Limit). Scalar
// operands broadcast, same-size containers apply element-wise.
inline uint8_t ScriptService::ExecVectorMath(LoadedScript *Script, const ScriptLineInfo &Ln, const uint8_t *Opbase,
                                    uint8_t Cat, uint8_t Op, uint16_t Dtype, uint8_t *Dest, uint8_t Dsize) {
    // Only the element-wise instructions that remain (Modulo/Minimum/Maximum/Absolute/Limit);
    // arithmetic on a container goes through the Set expression.
    if (Cat != SCRIPT_CAT_MATH) return SCRIPT_ERR_TYPE;
    if (Op != SCRIPT_OP_MATH_MOD && Op != SCRIPT_OP_MATH_MIN && Op != SCRIPT_OP_MATH_MAX &&
        Op != SCRIPT_OP_MATH_ABS && Op != SCRIPT_OP_MATH_LIMIT)
        return SCRIPT_ERR_TYPE;

    uint16_t Count = ContainerCount(Dtype, Dsize, Dest);
    if (Count == 0) return SCRIPT_ERR_TYPE;
    uint8_t Off = (Dtype == (uint16_t)DataType::Matrix) ? 4 : 0;

    uint8_t OpCount = Ln.OpCount;
    if (OpCount > 8) OpCount = 8;
    if (OpCount < 1) return SCRIPT_ERR_OPERAND;
    const uint8_t *Odata[8];
    uint8_t Osize[8];
    uint16_t Otype[8];
    uint8_t Oscratch[8][4];
    for (uint8_t i = 0; i < OpCount; i++) {
        if (!Resolve(Script, Opbase[(size_t)i * 4], Opbase[(size_t)i * 4 + 1],
                           SymVal(Opbase + (size_t)i * 4), Oscratch[i],
                           &Odata[i], &Osize[i], &Otype[i]))
            return SCRIPT_ERR_OPERAND;
    }

    for (uint32_t Index = 0; Index < Count; Index++) {
        Number Acc;
        if (!VectorElement(Odata[0], Osize[0], Otype[0], Index, Acc)) return SCRIPT_ERR_TYPE;
        if (Op == SCRIPT_OP_MATH_LIMIT) {
            Number Lo, Hi;
            if (OpCount < 3) return SCRIPT_ERR_OPERAND;
            if (!VectorElement(Odata[1], Osize[1], Otype[1], Index, Lo)) return SCRIPT_ERR_TYPE;
            if (!VectorElement(Odata[2], Osize[2], Otype[2], Index, Hi)) return SCRIPT_ERR_TYPE;
            if (Acc < Lo) Acc = Lo;
            if (Acc > Hi) Acc = Hi;
        } else if (Op == SCRIPT_OP_MATH_ABS) {
            if (OpCount != 1) return SCRIPT_ERR_TYPE; // Absolute is unary
            Acc = abs(Acc);
        } else {
            for (uint8_t k = 1; k < OpCount; k++) {
                Number Elem;
                if (!VectorElement(Odata[k], Osize[k], Otype[k], Index, Elem)) return SCRIPT_ERR_TYPE;
                switch (Op) {
                    case SCRIPT_OP_MATH_MOD: {
                        int32_t Divisor = Elem.RoundToInt();
                        if (Divisor == 0) return SCRIPT_ERR_TYPE;
                        Acc = Number(Acc.RoundToInt() % Divisor);
                        break;
                    }
                    case SCRIPT_OP_MATH_MIN: Acc = min(Acc, Elem); break;
                    case SCRIPT_OP_MATH_MAX: Acc = max(Acc, Elem); break;
                    default: return SCRIPT_ERR_TYPE;
                }
            }
        }
        int32_t Raw = Acc.Value;
        memcpy(Dest + Off + Index * 4, &Raw, 4);
    }
    Script->Ic++;
    return SCRIPT_ERR_NONE;
}


#endif // USE_SCRIPTS
