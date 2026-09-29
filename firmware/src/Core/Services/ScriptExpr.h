#pragma once

// Infix expression evaluation (the Set instruction) (ScriptExpr.h) - part of the script service.
//
// Scalar, vector and matrix expressions, element-wise with scalar broadcast.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Types/Number.h"

// ---------------------------------------------------------------------------
// Infix expression evaluation (the Set instruction): scalar, vector and matrix
// values, element-wise with scalar broadcast (Docs/Services/Script.md: "generic math
// and logic processor"). Limit/Min/Max stay separate instructions.
// ---------------------------------------------------------------------------

#define SCRIPT_EXPR_MAX_ELEMS 9 // covers Number, Vector2/3, Matrix2x3, Colour
#define SCRIPT_EXPR_MAX_DEPTH 8 // paren/`^` nesting cap (keeps the VM stack bounded)

struct ExprValue
{
    uint16_t dtype = (uint16_t)DataType::None;
    uint16_t mh = 0, mw = 0; // Matrix header (0 otherwise)
    uint8_t count = 0;       // elements (1 = scalar)
    Number e[SCRIPT_EXPR_MAX_ELEMS];
};

struct ExprParser
{
    LoadedScript *s;
    const uint8_t *tok;
    uint8_t n;
    uint8_t i;
    uint8_t depth;
    uint8_t scratch[4];
};

// Resolves a value token into an ExprValue (scalar / Vector / Matrix).
static bool ScriptExprResolve(ExprParser *p, const uint8_t *sym, ExprValue &out)
{
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(p->s, sym[0], sym[1], ScriptSymVal(sym), p->scratch, &data, &size, &dtype))
        return false;
    out.dtype = dtype;
    out.mh = 0;
    out.mw = 0;

    uint16_t cnt = 1;
    if (dtype == (uint16_t)DataType::Vector || dtype == (uint16_t)DataType::Matrix)
    {
        cnt = ScriptContainerCount(dtype, size, data);
        if (cnt == 0 || cnt > SCRIPT_EXPR_MAX_ELEMS) return false;
        if (dtype == (uint16_t)DataType::Matrix)
        {
            out.mh = (uint16_t)(data[0] | (data[1] << 8));
            out.mw = (uint16_t)(data[2] | (data[3] << 8));
        }
    }
    for (uint16_t i = 0; i < cnt; i++)
        if (!ScriptVectorElement(data, size, dtype, i, out.e[i])) return false;
    out.count = (uint8_t)cnt;
    return true;
}

// `Math op` value at the cursor, or 0xFF when the next token is a value / end.
static uint8_t ScriptExprPeekOp(ExprParser *p)
{
    if (p->i >= p->n) return 0xFF;
    const uint8_t *sym = p->tok + (size_t)p->i * 4;
    if (sym[0] == SCRIPT_SYM_PREDEFINE && sym[1] == SCRIPT_PRE_MATHOP)
        return (uint8_t)(ScriptSymVal(sym) & 0xFF);
    return 0xFF;
}

// base ^ exp. Fast paths: exp == 0.5 -> sqrt, integer exp -> repeated multiply (negative ->
// reciprocal). Any other exponent uses its binary expansion: with exp = int + sum(2^-k) over
// the set fraction bits, x^exp = x^int * prod(x^(2^-k)), and x^(2^-k) is k nested square
// roots. Fixed point end to end (no log/exp tables); the chain is only deepened while a lower
// fraction bit still needs it.
static bool ScriptExprPow(Number base, Number exp, Number &out)
{
    if (exp.Value == (int32_t)(1 << 15)) { out = sqrt(base); return true; } // 0.5

    bool neg = exp.Value < 0;
    if (neg) exp = -exp;
    int32_t ip = exp.Value >> 16;      // integer part
    int32_t frac = exp.Value & 0xFFFF; // Q16.16 fraction
    if (ip > 64) return false;
    if (frac && base.Value < 0) return false; // no real fractional power of a negative base

    Number r = Number(1);
    for (int32_t i = 0; i < ip; i++) r = r * base;
    if (frac)
    {
        Number chain = sqrt(base); // base^(2^-1)
        int32_t bit = 0x8000;
        while (bit)
        {
            if (frac & bit) r = r * chain;
            bit >>= 1;
            // The chain must track the next bit's exponent; only skip a sqrt when no bit at
            // or below the new one is set (otherwise it would be one level too shallow).
            if (bit && (frac & ((bit << 1) - 1))) chain = sqrt(chain);
        }
    }
    if (neg) { if (r.Value == 0) return false; r = Number(1) / r; }
    out = r;
    return true;
}

// Comparisons and logic ops (scalar operands -> Bool).
static bool ScriptExprIsBoolOp(uint8_t op)
{
    return op == SCRIPT_MATHOP_EQ || op == SCRIPT_MATHOP_NE || op == SCRIPT_MATHOP_LT ||
           op == SCRIPT_MATHOP_LE || op == SCRIPT_MATHOP_GT || op == SCRIPT_MATHOP_GE ||
           op == SCRIPT_MATHOP_AND || op == SCRIPT_MATHOP_OR || op == SCRIPT_MATHOP_XOR;
}

// a = a <op> b, element-wise (a scalar broadcasts to a container's shape). Comparisons and
// logic produce a scalar Bool and require scalar operands.
static bool ScriptExprApply(ExprValue &a, const ExprValue &b, uint8_t op)
{
    if (ScriptExprIsBoolOp(op))
    {
        if (a.count != 1 || b.count != 1) return false;
        int32_t x = a.e[0].Value, y = b.e[0].Value;
        bool r;
        switch (op)
        {
        case SCRIPT_MATHOP_EQ: r = (x == y); break;
        case SCRIPT_MATHOP_NE: r = (x != y); break;
        case SCRIPT_MATHOP_LT: r = (x < y); break;
        case SCRIPT_MATHOP_LE: r = (x <= y); break;
        case SCRIPT_MATHOP_GT: r = (x > y); break;
        case SCRIPT_MATHOP_GE: r = (x >= y); break;
        case SCRIPT_MATHOP_AND: r = (x != 0) && (y != 0); break;
        case SCRIPT_MATHOP_OR: r = (x != 0) || (y != 0); break;
        case SCRIPT_MATHOP_XOR: r = (x != 0) != (y != 0); break;
        default: return false;
        }
        a.dtype = (uint16_t)DataType::Bool;
        a.mh = 0;
        a.mw = 0;
        a.count = 1;
        a.e[0] = Number(r ? 1 : 0);
        return true;
    }

    uint8_t count;
    if (a.count == b.count) count = a.count;
    else if (a.count == 1) count = b.count;
    else if (b.count == 1) count = a.count;
    else return false;
    const bool abcast = (a.count == 1);
    const Number asrc = abcast ? a.e[0] : Number(); // cached before the in-place writes
    if (abcast && b.count > 1) { a.dtype = b.dtype; a.mh = b.mh; a.mw = b.mw; }
    a.count = count;
    for (uint8_t k = 0; k < count; k++)
    {
        Number va = abcast ? asrc : a.e[k];
        Number vb = (b.count == 1) ? b.e[0] : b.e[k];
        switch (op)
        {
        case SCRIPT_MATHOP_ADD: va = va + vb; break;
        case SCRIPT_MATHOP_SUB: va = va - vb; break;
        case SCRIPT_MATHOP_MUL: va = va * vb; break;
        case SCRIPT_MATHOP_DIV: if (vb.Value == 0) return false; va = va / vb; break;
        case SCRIPT_MATHOP_MOD: {
            int32_t b = vb.RoundToInt();
            if (b == 0) return false;
            va = Number(va.RoundToInt() % b);
            break;
        }
        case SCRIPT_MATHOP_POW: if (!ScriptExprPow(va, vb, va)) return false; break;
        default: return false;
        }
        a.e[k] = va;
    }
    return true;
}

static bool ScriptExprBin(ExprParser *p, ExprValue &out, uint8_t minPrec);

static bool ScriptExprPrimary(ExprParser *p, ExprValue &out)
{
    if (p->depth >= SCRIPT_EXPR_MAX_DEPTH) return false;
    p->depth++;
    bool ok;
    if (ScriptExprPeekOp(p) == SCRIPT_MATHOP_OPEN)
    {
        p->i++;
        ok = ScriptExprBin(p, out, 1);
        if (ok)
        {
            if (ScriptExprPeekOp(p) != SCRIPT_MATHOP_CLOSE) ok = false;
            else p->i++;
        }
    }
    else if (p->i < p->n)
    {
        const uint8_t *sym = p->tok + (size_t)p->i * 4;
        ok = ScriptExprResolve(p, sym, out);
        if (ok) p->i++;
    }
    else
    {
        ok = false;
    }
    p->depth--;
    return ok;
}

// dot a b: sum(a_i * b_i) -> Number.
static bool ScriptExprFnDot(const ExprValue &a, const ExprValue &b, ExprValue &out)
{
    if (a.count == 0 || a.count != b.count) return false;
    Number s = Number(0);
    for (uint8_t k = 0; k < a.count; k++) s = s + a.e[k] * b.e[k];
    out.dtype = (uint16_t)DataType::Number;
    out.mh = 0;
    out.mw = 0;
    out.count = 1;
    out.e[0] = s;
    return true;
}// transpose m: Matrix R x C -> C x R.
static bool ScriptExprFnTranspose(const ExprValue &a, ExprValue &out)
{
    if (a.dtype != (uint16_t)DataType::Matrix || a.mh == 0 || a.mw == 0) return false;
    uint16_t R = a.mh, C = a.mw;
    if ((uint32_t)R * C > SCRIPT_EXPR_MAX_ELEMS) return false;
    out.dtype = (uint16_t)DataType::Matrix;
    out.mh = C;
    out.mw = R;
    out.count = a.count;
    for (uint16_t r = 0; r < R; r++)
        for (uint16_t c = 0; c < C; c++)
            out.e[c * R + r] = a.e[r * C + c];
    return true;
}// size v: Euclidean norm -> Number, i.e. sqrt(dot(v, v)).
static bool ScriptExprFnSize(const ExprValue &a, ExprValue &out)
{
    // A resolved value always has at least one element, so dot's non-empty guard never
    // rejects a size().
    if (!ScriptExprFnDot(a, a, out)) return false;
    out.e[0] = sqrt(out.e[0]);
    return true;
}

// cross a b: Vector3 x Vector3 -> Vector3.
static bool ScriptExprFnCross(const ExprValue &a, const ExprValue &b, ExprValue &out)
{
    if (a.count != 3 || b.count != 3) return false;
    out.dtype = (uint16_t)DataType::Vector;
    out.mh = 0;
    out.mw = 0;
    out.count = 3;
    out.e[0] = a.e[1] * b.e[2] - a.e[2] * b.e[1];
    out.e[1] = a.e[2] * b.e[0] - a.e[0] * b.e[2];
    out.e[2] = a.e[0] * b.e[1] - a.e[1] * b.e[0];
    return true;
}

static bool ScriptExprUnary(ExprParser *p, ExprValue &out)
{
    uint8_t op = ScriptExprPeekOp(p);
    if (op == SCRIPT_MATHOP_FN_SIZE || op == SCRIPT_MATHOP_FN_TRANSPOSE)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        return (op == SCRIPT_MATHOP_FN_SIZE) ? ScriptExprFnSize(a, out)
                                             : ScriptExprFnTranspose(a, out);
    }
    if (op == SCRIPT_MATHOP_FN_DOT || op == SCRIPT_MATHOP_FN_CROSS)
    {
        p->i++;
        ExprValue a, b;
        if (!ScriptExprUnary(p, a)) return false;
        if (!ScriptExprUnary(p, b)) return false;
        return (op == SCRIPT_MATHOP_FN_DOT) ? ScriptExprFnDot(a, b, out)
                                            : ScriptExprFnCross(a, b, out);
    }
    if (op == SCRIPT_MATHOP_SUB)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        for (uint8_t k = 0; k < a.count; k++) a.e[k] = -a.e[k];
        out = a;
        return true;
    }
    if (op == SCRIPT_MATHOP_NOT)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        if (a.count != 1) return false;
        out.dtype = (uint16_t)DataType::Bool;
        out.mh = 0;
        out.mw = 0;
        out.count = 1;
        out.e[0] = Number(a.e[0].Value == 0 ? 1 : 0);
        return true;
    }
    return ScriptExprPrimary(p, out);
}

// Precedence climbing: MUL/DIV bind tighter than ADD/SUB; POW is right-associative.
static bool ScriptExprBin(ExprParser *p, ExprValue &out, uint8_t minPrec)
{
    if (!ScriptExprUnary(p, out)) return false;
    for (;;)
    {
        uint8_t op = ScriptExprPeekOp(p);
        uint8_t prec = (op == SCRIPT_MATHOP_POW) ? 9
                       : (op == SCRIPT_MATHOP_MUL || op == SCRIPT_MATHOP_DIV ||
                          op == SCRIPT_MATHOP_MOD) ? 8
                       : (op == SCRIPT_MATHOP_ADD || op == SCRIPT_MATHOP_SUB) ? 7
                       : (op == SCRIPT_MATHOP_LT || op == SCRIPT_MATHOP_LE ||
                          op == SCRIPT_MATHOP_GT || op == SCRIPT_MATHOP_GE) ? 5
                       : (op == SCRIPT_MATHOP_EQ || op == SCRIPT_MATHOP_NE) ? 4
                       : (op == SCRIPT_MATHOP_AND) ? 3
                       : (op == SCRIPT_MATHOP_XOR) ? 2
                       : (op == SCRIPT_MATHOP_OR) ? 1 : 0;
        if (prec == 0 || prec < minPrec) break;
        p->i++;
        ExprValue b;
        uint8_t next = (op == SCRIPT_MATHOP_POW) ? prec : (uint8_t)(prec + 1);
        if (!ScriptExprBin(p, b, next)) return false;
        if (!ScriptExprApply(out, b, op)) return false;
    }
    return true;
}

#endif // USE_SCRIPTS
