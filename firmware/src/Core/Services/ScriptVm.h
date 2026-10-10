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
    Number n;
    int32_t i;
};

// SetState is declared as a ScriptService member (Script.h) and defined in ScriptRuntime.h;
// the Script-state opcode uses it so self state changes follow the same transition rules
// (IC reset, pending-confirmation clearing) as CID 4.

inline uint16_t ScriptService::SymVal(const uint8_t *Sym) { return (uint16_t)(Sym[2] | (Sym[3] << 8)); }

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

inline int32_t ScriptService::LoadInt(const uint8_t *d, uint8_t Sz, bool Sign) {
    int32_t v = 0;
    for (uint8_t i = 0; i < Sz && i < 4; i++) v |= (int32_t)d[i] << (8 * i);
    if (Sign && Sz > 0 && Sz < 4 && (d[Sz - 1] & 0x80)) v |= ~((1 << (8 * Sz)) - 1);
    return v;
}

inline void ScriptService::StoreInt(uint8_t *d, uint8_t Sz, int32_t v) {
    for (uint8_t i = 0; i < Sz && i < 4; i++) d[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    for (uint8_t i = 4; i < Sz; i++) d[i] = 0;
}

// Resolves a source symbol into bytes. Predefines are materialised into `scratch`.
inline bool ScriptService::Resolve(LoadedScript *s, uint8_t Stype, uint8_t Ssub, uint16_t Sval,
                          uint8_t *Scratch, const uint8_t **Data, uint8_t *Size, uint16_t *Dtype) {
    switch (Stype) {
        case SCRIPT_SYM_INPUT:
            if (Sval >= s->inCount || !s->ioSpace) return false;
            *Data = s->ioSpace + s->InputOffset((uint8_t)Sval);
            *Size = s->inMeta[Sval].Size;
            *Dtype = ValueInfoType(s->inMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_OUTPUT:
            if (Sval >= s->outCount || !s->ioSpace) return false;
            *Data = s->ioSpace + s->OutputOffset((uint8_t)Sval);
            *Size = s->outMeta[Sval].Size;
            *Dtype = ValueInfoType(s->outMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_VARIABLE:
            if (Sval >= s->varCount) return false;
            *Data = s->varSpace + s->VarOffset((uint8_t)Sval);
            *Size = s->varMeta[Sval].Size;
            *Dtype = ValueInfoType(s->varMeta[Sval].Type);
            return true;
        case SCRIPT_SYM_CONSTANT:
            if (Sval >= s->constCount) return false;
            *Data = s->constSpace + s->ConstOffset((uint8_t)Sval);
            *Size = s->constMeta[Sval].Size;
            *Dtype = ValueInfoType(s->constMeta[Sval].Type);
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

inline bool ScriptService::ToScalar(const uint8_t *Data, uint8_t Size, uint16_t Dtype, bool Num, ScriptScalar &Out) {
    Out.n = Number(0);
    Out.i = 0;
    if (Num) {
        switch (Dtype) {
            case (uint16_t)DataType::Number: {
                if (Size < 4) return false;
                int32_t Raw = 0; memcpy(&Raw, Data, 4); Out.n = Number::FromRaw(Raw); return true;
            }
            case (uint16_t)DataType::Index: Out.n = Number(LoadInt(Data, Size, true)); return true;
            case (uint16_t)DataType::Uint32: Out.n = Number((int32_t)LoadInt(Data, Size, false)); return true;
            case (uint16_t)DataType::Bool: Out.n = Number(Data[0] ? 1 : 0); return true;
            case (uint16_t)DataType::Enum: Out.n = Number((int32_t)LoadInt(Data, Size, false)); return true;
            case (uint16_t)DataType::DevType: Out.n = Number((int32_t)LoadInt(Data, Size, false)); return true;
            case (uint16_t)DataType::Id: Out.n = Number((int32_t)LoadInt(Data, Size, false)); return true;
            default: return false;
        }
    }
    switch (Dtype) {
        case (uint16_t)DataType::Number: {
            if (Size < 4) return false;
            int32_t Raw = 0; memcpy(&Raw, Data, 4); Out.i = Number::FromRaw(Raw).ToInt(); return true;
        }
        case (uint16_t)DataType::Index: Out.i = LoadInt(Data, Size, true); return true;
        case (uint16_t)DataType::Uint32: Out.i = LoadInt(Data, Size, false); return true;
        case (uint16_t)DataType::Bool: Out.i = Data[0] ? 1 : 0; return true;
        case (uint16_t)DataType::Enum: Out.i = LoadInt(Data, Size, false); return true;
        case (uint16_t)DataType::DevType: Out.i = LoadInt(Data, Size, false); return true;
        case (uint16_t)DataType::Id: Out.i = LoadInt(Data, Size, false); return true;
        default: return false;
    }
}

inline bool ScriptService::ResolveOperandScalar(LoadedScript *s, const uint8_t *Sym, bool Num, ScriptScalar &Out) {
    uint8_t Scratch[4];
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(s, Sym[0], Sym[1], SymVal(Sym), Scratch, &Data, &Size, &Dtype)) return false;
    return ToScalar(Data, Size, Dtype, Num, Out);
}

inline bool ScriptService::ResolveOperandInt(LoadedScript *s, const uint8_t *Sym, int32_t &Out) {
    ScriptScalar Sc;
    if (!ResolveOperandScalar(s, Sym, false, Sc)) return false;
    Out = Sc.i;
    return true;
}

// Resolves a destination symbol (variable or output) into its storage.
inline bool ScriptService::ResolveDest(LoadedScript *s, const uint8_t *Sym, uint16_t &Dtype, uint8_t *&Data, uint8_t &Size) {
    if (Sym[0] == SCRIPT_SYM_VARIABLE) {
        uint16_t Idx = SymVal(Sym);
        if (Idx >= s->varCount) return false;
        Data = s->varSpace + s->VarOffset((uint8_t)Idx);
        Size = s->varMeta[Idx].Size;
        Dtype = ValueInfoType(s->varMeta[Idx].Type);
        return true;
    }
    if (Sym[0] == SCRIPT_SYM_OUTPUT) {
        uint16_t Idx = SymVal(Sym);
        if (Idx >= s->outCount || !s->ioSpace) return false;
        Data = s->ioSpace + s->OutputOffset((uint8_t)Idx);
        Size = s->outMeta[Idx].Size;
        Dtype = ValueInfoType(s->outMeta[Idx].Type);
        return true;
    }
    return false;
}

inline void ScriptService::StoreScalar(uint16_t Dtype, uint8_t *Data, uint8_t Size, bool Num, const ScriptScalar &v) {
    switch (Dtype) {
        case (uint16_t)DataType::Number: {
            int32_t Raw = Num ? v.n.Value : Number(v.i).Value;
            memcpy(Data, &Raw, 4);
            break;
        }
        case (uint16_t)DataType::Bool: {
            int32_t Iv = Num ? v.n.RoundToInt() : v.i;
            Data[0] = Iv ? 1 : 0;
            break;
        }
        default: {
            int32_t Iv = Num ? v.n.RoundToInt() : v.i;
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
        uint8_t n = Ssize < Dsize ? Ssize : Dsize;
        if (n) memcpy(Dest, Src, n);
        for (uint8_t i = n; i < Dsize; i++) Dest[i] = 0;
    }
    return SCRIPT_ERR_NONE;
}

inline uint8_t ScriptService::Assign(LoadedScript *s, const uint8_t *destSym, const uint8_t *Src,
                            uint8_t Ssize, uint16_t Stype) {
    uint16_t Dtype = 0;
    uint8_t *Dest = nullptr;
    uint8_t Dsize = 0;
    if (!ResolveDest(s, destSym, Dtype, Dest, Dsize)) return SCRIPT_ERR_OPERAND;
    return AssignResolved(Dtype, Dest, Dsize, Src, Ssize, Stype);
}

// Number of elements in a Vector/Matrix value (0 for scalars / malformed containers).
inline uint16_t ScriptService::ContainerCount(uint16_t Dtype, uint8_t Size, const uint8_t *Data) {
    if (Dtype == (uint16_t)DataType::Vector) return Size / 4;
    if (Dtype == (uint16_t)DataType::Matrix) {
        if (Size < 4 || !Data) return 0;
        uint16_t h = (uint16_t)(Data[0] | (Data[1] << 8));
        uint16_t w = (uint16_t)(Data[2] | (Data[3] << 8));
        uint32_t n = (uint32_t)h * w;
        if (n > (uint32_t)(Size - 4) / 4) return 0;
        return (uint16_t)n;
    }
    return 0;
}

// Reads element `e` of a value as a Number. Scalars broadcast (the same value for any e).
inline bool ScriptService::VectorElement(const uint8_t *Data, uint8_t Size, uint16_t Dtype, uint32_t e, Number &Out) {
    int32_t Raw = 0;
    switch (Dtype) {
        case (uint16_t)DataType::Number:
            if (Size < 4) return false;
            memcpy(&Raw, Data, 4); Out = Number::FromRaw(Raw); return true;
        case (uint16_t)DataType::Index: Out = Number(LoadInt(Data, Size, true)); return true;
        case (uint16_t)DataType::Uint32: Out = Number((int32_t)LoadInt(Data, Size, false)); return true;
        case (uint16_t)DataType::Bool: Out = Number(Data[0] ? 1 : 0); return true;
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id: Out = Number((int32_t)LoadInt(Data, Size, false)); return true;
        case (uint16_t)DataType::Vector:
            if ((e + 1) * 4 > Size) return false;
            memcpy(&Raw, Data + e * 4, 4); Out = Number::FromRaw(Raw); return true;
        case (uint16_t)DataType::Matrix:
            if (e >= ContainerCount(Dtype, Size, Data)) return false;
            memcpy(&Raw, Data + 4 + e * 4, 4); Out = Number::FromRaw(Raw); return true;
        default: return false;
    }
}

// Element-wise math on a Vector/Matrix destination (Mod/Min/Max/Abs/Limit). Scalar
// operands broadcast, same-size containers apply element-wise.
inline uint8_t ScriptService::ExecVectorMath(LoadedScript *s, const ScriptLineInfo &Ln, const uint8_t *Opbase,
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

    uint8_t n = Ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    const uint8_t *Odata[8];
    uint8_t Osize[8];
    uint16_t Otype[8];
    uint8_t Oscratch[8][4];
    for (uint8_t i = 0; i < n; i++) {
        if (!Resolve(s, Opbase[(size_t)i * 4], Opbase[(size_t)i * 4 + 1],
                           SymVal(Opbase + (size_t)i * 4), Oscratch[i],
                           &Odata[i], &Osize[i], &Otype[i]))
            return SCRIPT_ERR_OPERAND;
    }

    for (uint32_t e = 0; e < Count; e++) {
        Number Acc;
        if (!VectorElement(Odata[0], Osize[0], Otype[0], e, Acc)) return SCRIPT_ERR_TYPE;
        if (Op == SCRIPT_OP_MATH_LIMIT) {
            Number Lo, Hi;
            if (n < 3) return SCRIPT_ERR_OPERAND;
            if (!VectorElement(Odata[1], Osize[1], Otype[1], e, Lo)) return SCRIPT_ERR_TYPE;
            if (!VectorElement(Odata[2], Osize[2], Otype[2], e, Hi)) return SCRIPT_ERR_TYPE;
            if (Acc < Lo) Acc = Lo;
            if (Acc > Hi) Acc = Hi;
        } else if (Op == SCRIPT_OP_MATH_ABS) {
            if (n != 1) return SCRIPT_ERR_TYPE; // Absolute is unary
            Acc = abs(Acc);
        } else {
            for (uint8_t k = 1; k < n; k++) {
                Number v;
                if (!VectorElement(Odata[k], Osize[k], Otype[k], e, v)) return SCRIPT_ERR_TYPE;
                switch (Op) {
                    case SCRIPT_OP_MATH_MOD: {
                        int32_t b = v.RoundToInt();
                        if (b == 0) return SCRIPT_ERR_TYPE;
                        Acc = Number(Acc.RoundToInt() % b);
                        break;
                    }
                    case SCRIPT_OP_MATH_MIN: Acc = min(Acc, v); break;
                    case SCRIPT_OP_MATH_MAX: Acc = max(Acc, v); break;
                    default: return SCRIPT_ERR_TYPE;
                }
            }
        }
        int32_t Raw = Acc.Value;
        memcpy(Dest + Off + e * 4, &Raw, 4);
    }
    s->Ic++;
    return SCRIPT_ERR_NONE;
}


#endif // USE_SCRIPTS
