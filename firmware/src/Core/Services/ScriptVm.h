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

// Defined after the tick; used by the Script-state opcode so self state changes follow the
// same transition rules (IC reset, pending-confirmation clearing) as CID 4.
static void ScriptSetState(LoadedScript *s, uint8_t newState);

static inline uint16_t ScriptSymVal(const uint8_t *sym) { return (uint16_t)(sym[2] | (sym[3] << 8)); }

static inline bool ScriptIsNumericDtype(uint16_t dtype) {
    switch (dtype) {
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

static int32_t ScriptLoadInt(const uint8_t *d, uint8_t sz, bool sign) {
    int32_t v = 0;
    for (uint8_t i = 0; i < sz && i < 4; i++) v |= (int32_t)d[i] << (8 * i);
    if (sign && sz > 0 && sz < 4 && (d[sz - 1] & 0x80)) v |= ~((1 << (8 * sz)) - 1);
    return v;
}

static void ScriptStoreInt(uint8_t *d, uint8_t sz, int32_t v) {
    for (uint8_t i = 0; i < sz && i < 4; i++) d[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    for (uint8_t i = 4; i < sz; i++) d[i] = 0;
}

// Resolves a source symbol into bytes. Predefines are materialised into `scratch`.
static bool ScriptResolve(LoadedScript *s, uint8_t stype, uint8_t ssub, uint16_t sval,
                          uint8_t *scratch, const uint8_t **data, uint8_t *size, uint16_t *dtype) {
    switch (stype) {
        case SCRIPT_SYM_INPUT:
            if (sval >= s->inCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->InputOffset((uint8_t)sval);
            *size = s->inMeta[sval].Size;
            *dtype = ValueInfoType(s->inMeta[sval].Type);
            return true;
        case SCRIPT_SYM_OUTPUT:
            if (sval >= s->outCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->OutputOffset((uint8_t)sval);
            *size = s->outMeta[sval].Size;
            *dtype = ValueInfoType(s->outMeta[sval].Type);
            return true;
        case SCRIPT_SYM_VARIABLE:
            if (sval >= s->varCount) return false;
            *data = s->varSpace + s->VarOffset((uint8_t)sval);
            *size = s->varMeta[sval].Size;
            *dtype = ValueInfoType(s->varMeta[sval].Type);
            return true;
        case SCRIPT_SYM_CONSTANT:
            if (sval >= s->constCount) return false;
            *data = s->constSpace + s->ConstOffset((uint8_t)sval);
            *size = s->constMeta[sval].Size;
            *dtype = ValueInfoType(s->constMeta[sval].Type);
            return true;
        case SCRIPT_SYM_PREDEFINE:
            memset(scratch, 0, 4);
            switch (ssub) {
                case SCRIPT_PRE_INDEX: scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Index; break;
                case SCRIPT_PRE_CHAR:  scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Uint32; break;
                case SCRIPT_PRE_BOOL:  scratch[0] = sval ? 1 : 0; *size = 1; *dtype = (uint16_t)DataType::Bool; break;
                case SCRIPT_PRE_STATE: scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_MATHOP:scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_TYPE:  scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_NUMBER: {
                    // 16-bit Q8.8 literal -> 16.16 Number (e.g. 128 -> 0.5).
                    int32_t raw = (int32_t)(int16_t)sval << 8;
                    memcpy(scratch, &raw, 4);
                    *size = 4; *dtype = (uint16_t)DataType::Number; break;
                }
                default: return false;
            }
            *data = scratch;
            return true;
        default:
            return false;
    }
}

static bool ScriptToScalar(const uint8_t *data, uint8_t size, uint16_t dtype, bool num, ScriptScalar &out) {
    out.n = Number(0);
    out.i = 0;
    if (num) {
        switch (dtype) {
            case (uint16_t)DataType::Number: {
                if (size < 4) return false;
                int32_t raw = 0; memcpy(&raw, data, 4); out.n = Number::FromRaw(raw); return true;
            }
            case (uint16_t)DataType::Index: out.n = Number(ScriptLoadInt(data, size, true)); return true;
            case (uint16_t)DataType::Uint32: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Bool: out.n = Number(data[0] ? 1 : 0); return true;
            case (uint16_t)DataType::Enum: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::DevType: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Id: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            default: return false;
        }
    }
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            if (size < 4) return false;
            int32_t raw = 0; memcpy(&raw, data, 4); out.i = Number::FromRaw(raw).ToInt(); return true;
        }
        case (uint16_t)DataType::Index: out.i = ScriptLoadInt(data, size, true); return true;
        case (uint16_t)DataType::Uint32: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Bool: out.i = data[0] ? 1 : 0; return true;
        case (uint16_t)DataType::Enum: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::DevType: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Id: out.i = ScriptLoadInt(data, size, false); return true;
        default: return false;
    }
}

static bool ScriptResolveOperandScalar(LoadedScript *s, const uint8_t *sym, bool num, ScriptScalar &out) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    return ScriptToScalar(data, size, dtype, num, out);
}

static bool ScriptResolveOperandInt(LoadedScript *s, const uint8_t *sym, int32_t &out) {
    ScriptScalar sc;
    if (!ScriptResolveOperandScalar(s, sym, false, sc)) return false;
    out = sc.i;
    return true;
}

// Resolves a destination symbol (variable or output) into its storage.
static bool ScriptResolveDest(LoadedScript *s, const uint8_t *sym, uint16_t &dtype, uint8_t *&data, uint8_t &size) {
    if (sym[0] == SCRIPT_SYM_VARIABLE) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->varCount) return false;
        data = s->varSpace + s->VarOffset((uint8_t)idx);
        size = s->varMeta[idx].Size;
        dtype = ValueInfoType(s->varMeta[idx].Type);
        return true;
    }
    if (sym[0] == SCRIPT_SYM_OUTPUT) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->outCount || !s->ioSpace) return false;
        data = s->ioSpace + s->OutputOffset((uint8_t)idx);
        size = s->outMeta[idx].Size;
        dtype = ValueInfoType(s->outMeta[idx].Type);
        return true;
    }
    return false;
}

static void ScriptStoreScalar(uint16_t dtype, uint8_t *data, uint8_t size, bool num, const ScriptScalar &v) {
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            int32_t raw = num ? v.n.Value : Number(v.i).Value;
            memcpy(data, &raw, 4);
            break;
        }
        case (uint16_t)DataType::Bool: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            data[0] = iv ? 1 : 0;
            break;
        }
        default: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            ScriptStoreInt(data, size, iv);
            break;
        }
    }
}

// Stores a resolved source into an already-resolved destination (numeric conversion when
// both sides are numeric, raw clamped copy otherwise).
static uint8_t ScriptAssignResolved(uint16_t dtype, uint8_t *dest, uint8_t dsize,
                                    const uint8_t *src, uint8_t ssize, uint16_t stype) {
    if (ScriptIsNumericDtype(dtype) && ScriptIsNumericDtype(stype)) {
        ScriptScalar sc;
        bool num = (dtype == (uint16_t)DataType::Number);
        if (!ScriptToScalar(src, ssize, stype, num, sc)) return SCRIPT_ERR_TYPE;
        ScriptStoreScalar(dtype, dest, dsize, num, sc);
    } else {
        uint8_t n = ssize < dsize ? ssize : dsize;
        if (n) memcpy(dest, src, n);
        for (uint8_t i = n; i < dsize; i++) dest[i] = 0;
    }
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptAssign(LoadedScript *s, const uint8_t *destSym, const uint8_t *src,
                            uint8_t ssize, uint16_t stype) {
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, destSym, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    return ScriptAssignResolved(dtype, dest, dsize, src, ssize, stype);
}

// Number of elements in a Vector/Matrix value (0 for scalars / malformed containers).
static uint16_t ScriptContainerCount(uint16_t dtype, uint8_t size, const uint8_t *data) {
    if (dtype == (uint16_t)DataType::Vector) return size / 4;
    if (dtype == (uint16_t)DataType::Matrix) {
        if (size < 4 || !data) return 0;
        uint16_t h = (uint16_t)(data[0] | (data[1] << 8));
        uint16_t w = (uint16_t)(data[2] | (data[3] << 8));
        uint32_t n = (uint32_t)h * w;
        if (n > (uint32_t)(size - 4) / 4) return 0;
        return (uint16_t)n;
    }
    return 0;
}

// Reads element `e` of a value as a Number. Scalars broadcast (the same value for any e).
static bool ScriptVectorElement(const uint8_t *data, uint8_t size, uint16_t dtype, uint32_t e, Number &out) {
    int32_t raw = 0;
    switch (dtype) {
        case (uint16_t)DataType::Number:
            if (size < 4) return false;
            memcpy(&raw, data, 4); out = Number::FromRaw(raw); return true;
        case (uint16_t)DataType::Index: out = Number(ScriptLoadInt(data, size, true)); return true;
        case (uint16_t)DataType::Uint32: out = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
        case (uint16_t)DataType::Bool: out = Number(data[0] ? 1 : 0); return true;
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id: out = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
        case (uint16_t)DataType::Vector:
            if ((e + 1) * 4 > size) return false;
            memcpy(&raw, data + e * 4, 4); out = Number::FromRaw(raw); return true;
        case (uint16_t)DataType::Matrix:
            if (e >= ScriptContainerCount(dtype, size, data)) return false;
            memcpy(&raw, data + 4 + e * 4, 4); out = Number::FromRaw(raw); return true;
        default: return false;
    }
}

// Element-wise math on a Vector/Matrix destination (Mod/Min/Max/Abs/Limit). Scalar
// operands broadcast, same-size containers apply element-wise.
static uint8_t ScriptExecVectorMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                    uint8_t cat, uint8_t op, uint16_t dtype, uint8_t *dest, uint8_t dsize) {
    // Only the element-wise instructions that remain (Modulo/Minimum/Maximum/Absolute/Limit);
    // arithmetic on a container goes through the Set expression.
    if (cat != SCRIPT_CAT_MATH) return SCRIPT_ERR_TYPE;
    if (op != SCRIPT_OP_MATH_MOD && op != SCRIPT_OP_MATH_MIN && op != SCRIPT_OP_MATH_MAX &&
        op != SCRIPT_OP_MATH_ABS && op != SCRIPT_OP_MATH_LIMIT)
        return SCRIPT_ERR_TYPE;

    uint16_t count = ScriptContainerCount(dtype, dsize, dest);
    if (count == 0) return SCRIPT_ERR_TYPE;
    uint8_t off = (dtype == (uint16_t)DataType::Matrix) ? 4 : 0;

    uint8_t n = ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    const uint8_t *odata[8];
    uint8_t osize[8];
    uint16_t otype[8];
    uint8_t oscratch[8][4];
    for (uint8_t i = 0; i < n; i++) {
        if (!ScriptResolve(s, opbase[(size_t)i * 4], opbase[(size_t)i * 4 + 1],
                           ScriptSymVal(opbase + (size_t)i * 4), oscratch[i],
                           &odata[i], &osize[i], &otype[i]))
            return SCRIPT_ERR_OPERAND;
    }

    for (uint32_t e = 0; e < count; e++) {
        Number acc;
        if (!ScriptVectorElement(odata[0], osize[0], otype[0], e, acc)) return SCRIPT_ERR_TYPE;
        if (op == SCRIPT_OP_MATH_LIMIT) {
            Number lo, hi;
            if (n < 3) return SCRIPT_ERR_OPERAND;
            if (!ScriptVectorElement(odata[1], osize[1], otype[1], e, lo)) return SCRIPT_ERR_TYPE;
            if (!ScriptVectorElement(odata[2], osize[2], otype[2], e, hi)) return SCRIPT_ERR_TYPE;
            if (acc < lo) acc = lo;
            if (acc > hi) acc = hi;
        } else if (op == SCRIPT_OP_MATH_ABS) {
            if (n != 1) return SCRIPT_ERR_TYPE; // Absolute is unary
            acc = abs(acc);
        } else {
            for (uint8_t k = 1; k < n; k++) {
                Number v;
                if (!ScriptVectorElement(odata[k], osize[k], otype[k], e, v)) return SCRIPT_ERR_TYPE;
                switch (op) {
                    case SCRIPT_OP_MATH_MOD: {
                        int32_t b = v.RoundToInt();
                        if (b == 0) return SCRIPT_ERR_TYPE;
                        acc = Number(acc.RoundToInt() % b);
                        break;
                    }
                    case SCRIPT_OP_MATH_MIN: acc = min(acc, v); break;
                    case SCRIPT_OP_MATH_MAX: acc = max(acc, v); break;
                    default: return SCRIPT_ERR_TYPE;
                }
            }
        }
        int32_t raw = acc.Value;
        memcpy(dest + off + e * 4, &raw, 4);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}


#endif // USE_SCRIPTS
