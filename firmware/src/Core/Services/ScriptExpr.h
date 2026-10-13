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
    uint16_t Dtype = (uint16_t)DataType::None;
    uint16_t Mh = 0, Mw = 0; // Matrix header (0 otherwise)
    uint8_t Count = 0;       // elements (1 = scalar)
    Number Elems[SCRIPT_EXPR_MAX_ELEMS];
};

struct ExprParser
{
    LoadedScript *Script;
    const uint8_t *Tok;
    uint8_t Count;
    uint8_t Index;
    uint8_t Depth;
    uint8_t Scratch[4];
};

// Resolves a value token into an ExprValue (scalar / Vector / Matrix).
inline bool ScriptService::ExprResolve(ExprParser *Parser, const uint8_t *Sym, ExprValue &Out)
{
    const uint8_t *Data = nullptr;
    uint8_t Size = 0;
    uint16_t Dtype = 0;
    if (!Resolve(Parser->Script, Sym[0], Sym[1], SymVal(Sym), Parser->Scratch, &Data, &Size, &Dtype))
        return false;
    Out.Dtype = Dtype;
    Out.Mh = 0;
    Out.Mw = 0;

    uint16_t Cnt = 1;
    if (Dtype == (uint16_t)DataType::Vector || Dtype == (uint16_t)DataType::Matrix)
    {
        Cnt = ContainerCount(Dtype, Size, Data);
        if (Cnt == 0 || Cnt > SCRIPT_EXPR_MAX_ELEMS) return false;
        if (Dtype == (uint16_t)DataType::Matrix)
        {
            Out.Mh = ScriptRd16(Data);
            Out.Mw = ScriptRd16(Data + 2);
        }
    }
    for (uint16_t i = 0; i < Cnt; i++)
        if (!VectorElement(Data, Size, Dtype, i, Out.Elems[i])) return false;
    Out.Count = (uint8_t)Cnt;
    return true;
}

// `Math op` value at the cursor, or 0xFF when the next token is a value / end.
inline uint8_t ScriptService::ExprPeekOp(ExprParser *Parser)
{
    if (Parser->Index >= Parser->Count) return 0xFF;
    const uint8_t *Sym = Parser->Tok + (size_t)Parser->Index * 4;
    if (Sym[0] == SCRIPT_SYM_PREDEFINE && Sym[1] == SCRIPT_PRE_MATHOP)
        return (uint8_t)(SymVal(Sym) & 0xFF);
    return 0xFF;
}

// base ^ exp. Fast paths: exp == 0.5 -> sqrt, integer exp -> repeated multiply (negative ->
// reciprocal). Any other exponent uses its binary expansion: with exp = int + sum(2^-k) over
// the set fraction bits, x^exp = x^int * prod(x^(2^-k)), and x^(2^-k) is k nested square
// roots. Fixed point end to end (no log/exp tables); the chain is only deepened while a lower
// fraction bit still needs it.
inline bool ScriptService::ExprPow(Number Base, Number Exp, Number &Out)
{
    if (Exp.Value == (int32_t)(1 << 15)) { Out = sqrt(Base); return true; } // 0.5

    bool Neg = Exp.Value < 0;
    if (Neg) Exp = -Exp;
    int32_t Ip = Exp.Value >> 16;      // integer part
    int32_t Frac = Exp.Value & 0xFFFF; // Q16.16 fraction
    if (Ip > 64) return false;
    if (Frac && Base.Value < 0) return false; // no real fractional power of a negative base

    Number PowAcc = Number(1);
    for (int32_t i = 0; i < Ip; i++) PowAcc = PowAcc * Base;
    if (Frac)
    {
        Number Chain = sqrt(Base); // base^(2^-1)
        int32_t Bit = 0x8000;
        while (Bit)
        {
            if (Frac & Bit) PowAcc = PowAcc * Chain;
            Bit >>= 1;
            // The chain must track the next bit's exponent; only skip a sqrt when no bit at
            // or below the new one is set (otherwise it would be one level too shallow).
            if (Bit && (Frac & ((Bit << 1) - 1))) Chain = sqrt(Chain);
        }
    }
    if (Neg) { if (PowAcc.Value == 0) return false; PowAcc = Number(1) / PowAcc; }
    Out = PowAcc;
    return true;
}

// Comparisons and logic ops (scalar operands -> Bool).
inline bool ScriptService::ExprIsBoolOp(uint8_t Op)
{
    return Op == SCRIPT_MATHOP_EQ || Op == SCRIPT_MATHOP_NE || Op == SCRIPT_MATHOP_LT ||
           Op == SCRIPT_MATHOP_LE || Op == SCRIPT_MATHOP_GT || Op == SCRIPT_MATHOP_GE ||
           Op == SCRIPT_MATHOP_AND || Op == SCRIPT_MATHOP_OR || Op == SCRIPT_MATHOP_XOR;
}

// a = a <op> b, element-wise (a scalar broadcasts to a container's shape). Comparisons and
// logic produce a scalar Bool and require scalar operands.
inline bool ScriptService::ExprApply(ExprValue &Lhs, const ExprValue &Rhs, uint8_t Op)
{
    if (ExprIsBoolOp(Op))
    {
        if (Lhs.Count != 1 || Rhs.Count != 1) return false;
        int32_t x = Lhs.Elems[0].Value, y = Rhs.Elems[0].Value;
        bool Bool;
        switch (Op)
        {
        case SCRIPT_MATHOP_EQ: Bool = (x == y); break;
        case SCRIPT_MATHOP_NE: Bool = (x != y); break;
        case SCRIPT_MATHOP_LT: Bool = (x < y); break;
        case SCRIPT_MATHOP_LE: Bool = (x <= y); break;
        case SCRIPT_MATHOP_GT: Bool = (x > y); break;
        case SCRIPT_MATHOP_GE: Bool = (x >= y); break;
        case SCRIPT_MATHOP_AND: Bool = (x != 0) && (y != 0); break;
        case SCRIPT_MATHOP_OR: Bool = (x != 0) || (y != 0); break;
        case SCRIPT_MATHOP_XOR: Bool = (x != 0) != (y != 0); break;
        default: return false;
        }
        Lhs.Dtype = (uint16_t)DataType::Bool;
        Lhs.Mh = 0;
        Lhs.Mw = 0;
        Lhs.Count = 1;
        Lhs.Elems[0] = Number(Bool ? 1 : 0);
        return true;
    }

    uint8_t Count;
    if (Lhs.Count == Rhs.Count) Count = Lhs.Count;
    else if (Lhs.Count == 1) Count = Rhs.Count;
    else if (Rhs.Count == 1) Count = Lhs.Count;
    else return false;
    const bool Abcast = (Lhs.Count == 1);
    const Number Asrc = Abcast ? Lhs.Elems[0] : Number(); // cached before the in-place writes
    if (Abcast && Rhs.Count > 1) { Lhs.Dtype = Rhs.Dtype; Lhs.Mh = Rhs.Mh; Lhs.Mw = Rhs.Mw; }
    Lhs.Count = Count;
    for (uint8_t k = 0; k < Count; k++)
    {
        Number Va = Abcast ? Asrc : Lhs.Elems[k];
        Number Vb = (Rhs.Count == 1) ? Rhs.Elems[0] : Rhs.Elems[k];
        switch (Op)
        {
        case SCRIPT_MATHOP_ADD: Va = Va + Vb; break;
        case SCRIPT_MATHOP_SUB: Va = Va - Vb; break;
        case SCRIPT_MATHOP_MUL: Va = Va * Vb; break;
        case SCRIPT_MATHOP_DIV: if (Vb.Value == 0) return false; Va = Va / Vb; break;
        case SCRIPT_MATHOP_MOD: {
            int32_t Div = Vb.RoundToInt();
            if (Div == 0) return false;
            Va = Number(Va.RoundToInt() % Div);
            break;
        }
        case SCRIPT_MATHOP_POW: if (!ExprPow(Va, Vb, Va)) return false; break;
        default: return false;
        }
        Lhs.Elems[k] = Va;
    }
    return true;
}

inline bool ScriptService::ExprPrimary(ExprParser *Parser, ExprValue &Out)
{
    if (Parser->Depth >= SCRIPT_EXPR_MAX_DEPTH) return false;
    Parser->Depth++;
    bool Ok;
    if (ExprPeekOp(Parser) == SCRIPT_MATHOP_OPEN)
    {
        Parser->Index++;
        Ok = ExprBin(Parser, Out, 1);
        if (Ok)
        {
            if (ExprPeekOp(Parser) != SCRIPT_MATHOP_CLOSE) Ok = false;
            else Parser->Index++;
        }
    }
    else if (Parser->Index < Parser->Count)
    {
        const uint8_t *Sym = Parser->Tok + (size_t)Parser->Index * 4;
        Ok = ExprResolve(Parser, Sym, Out);
        if (Ok) Parser->Index++;
    }
    else
    {
        Ok = false;
    }
    Parser->Depth--;
    return Ok;
}

// dot a b: sum(a_i * b_i) -> Number.
inline bool ScriptService::ExprFnDot(const ExprValue &Lhs, const ExprValue &Rhs, ExprValue &Out)
{
    if (Lhs.Count == 0 || Lhs.Count != Rhs.Count) return false;
    Number Dot = Number(0);
    for (uint8_t k = 0; k < Lhs.Count; k++) Dot = Dot + Lhs.Elems[k] * Rhs.Elems[k];
    Out.Dtype = (uint16_t)DataType::Number;
    Out.Mh = 0;
    Out.Mw = 0;
    Out.Count = 1;
    Out.Elems[0] = Dot;
    return true;
}// transpose m: Matrix R x C -> C x R.
inline bool ScriptService::ExprFnTranspose(const ExprValue &Lhs, ExprValue &Out)
{
    if (Lhs.Dtype != (uint16_t)DataType::Matrix || Lhs.Mh == 0 || Lhs.Mw == 0) return false;
    uint16_t Rows = Lhs.Mh, Cols = Lhs.Mw;
    if ((uint32_t)Rows * Cols > SCRIPT_EXPR_MAX_ELEMS) return false;
    Out.Dtype = (uint16_t)DataType::Matrix;
    Out.Mh = Cols;
    Out.Mw = Rows;
    Out.Count = Lhs.Count;
    for (uint16_t Row = 0; Row < Rows; Row++)
        for (uint16_t Col = 0; Col < Cols; Col++)
            Out.Elems[Col * Rows + Row] = Lhs.Elems[Row * Cols + Col];
    return true;
}// size v: Euclidean norm -> Number, i.e. sqrt(dot(v, v)).
inline bool ScriptService::ExprFnSize(const ExprValue &Lhs, ExprValue &Out)
{
    // A resolved value always has at least one element, so dot's non-empty guard never
    // rejects a size().
    if (!ExprFnDot(Lhs, Lhs, Out)) return false;
    Out.Elems[0] = sqrt(Out.Elems[0]);
    return true;
}

// cross a b: Vector3 x Vector3 -> Vector3.
inline bool ScriptService::ExprFnCross(const ExprValue &Lhs, const ExprValue &Rhs, ExprValue &Out)
{
    if (Lhs.Count != 3 || Rhs.Count != 3) return false;
    Out.Dtype = (uint16_t)DataType::Vector;
    Out.Mh = 0;
    Out.Mw = 0;
    Out.Count = 3;
    Out.Elems[0] = Lhs.Elems[1] * Rhs.Elems[2] - Lhs.Elems[2] * Rhs.Elems[1];
    Out.Elems[1] = Lhs.Elems[2] * Rhs.Elems[0] - Lhs.Elems[0] * Rhs.Elems[2];
    Out.Elems[2] = Lhs.Elems[0] * Rhs.Elems[1] - Lhs.Elems[1] * Rhs.Elems[0];
    return true;
}

inline bool ScriptService::ExprUnary(ExprParser *Parser, ExprValue &Out)
{
    uint8_t Op = ExprPeekOp(Parser);
    if (Op == SCRIPT_MATHOP_FN_SIZE || Op == SCRIPT_MATHOP_FN_TRANSPOSE)
    {
        Parser->Index++;
        ExprValue Lhs;
        if (!ExprUnary(Parser, Lhs)) return false;
        return (Op == SCRIPT_MATHOP_FN_SIZE) ? ExprFnSize(Lhs, Out)
                                             : ExprFnTranspose(Lhs, Out);
    }
    if (Op == SCRIPT_MATHOP_FN_DOT || Op == SCRIPT_MATHOP_FN_CROSS)
    {
        Parser->Index++;
        ExprValue Lhs, Rhs;
        if (!ExprUnary(Parser, Lhs)) return false;
        if (!ExprUnary(Parser, Rhs)) return false;
        return (Op == SCRIPT_MATHOP_FN_DOT) ? ExprFnDot(Lhs, Rhs, Out)
                                            : ExprFnCross(Lhs, Rhs, Out);
    }
    if (Op == SCRIPT_MATHOP_SUB)
    {
        Parser->Index++;
        ExprValue Lhs;
        if (!ExprUnary(Parser, Lhs)) return false;
        for (uint8_t k = 0; k < Lhs.Count; k++) Lhs.Elems[k] = -Lhs.Elems[k];
        Out = Lhs;
        return true;
    }
    if (Op == SCRIPT_MATHOP_NOT)
    {
        Parser->Index++;
        ExprValue Lhs;
        if (!ExprUnary(Parser, Lhs)) return false;
        if (Lhs.Count != 1) return false;
        Out.Dtype = (uint16_t)DataType::Bool;
        Out.Mh = 0;
        Out.Mw = 0;
        Out.Count = 1;
        Out.Elems[0] = Number(Lhs.Elems[0].Value == 0 ? 1 : 0);
        return true;
    }
    return ExprPrimary(Parser, Out);
}

// Precedence climbing: MUL/DIV bind tighter than ADD/SUB; POW is right-associative.
inline bool ScriptService::ExprBin(ExprParser *Parser, ExprValue &Out, uint8_t MinPrec)
{
    if (!ExprUnary(Parser, Out)) return false;
    for (;;)
    {
        uint8_t Op = ExprPeekOp(Parser);
        uint8_t Prec = (Op == SCRIPT_MATHOP_POW) ? 9
                       : (Op == SCRIPT_MATHOP_MUL || Op == SCRIPT_MATHOP_DIV ||
                          Op == SCRIPT_MATHOP_MOD) ? 8
                       : (Op == SCRIPT_MATHOP_ADD || Op == SCRIPT_MATHOP_SUB) ? 7
                       : (Op == SCRIPT_MATHOP_LT || Op == SCRIPT_MATHOP_LE ||
                          Op == SCRIPT_MATHOP_GT || Op == SCRIPT_MATHOP_GE) ? 5
                       : (Op == SCRIPT_MATHOP_EQ || Op == SCRIPT_MATHOP_NE) ? 4
                       : (Op == SCRIPT_MATHOP_AND) ? 3
                       : (Op == SCRIPT_MATHOP_XOR) ? 2
                       : (Op == SCRIPT_MATHOP_OR) ? 1 : 0;
        if (Prec == 0 || Prec < MinPrec) break;
        Parser->Index++;
        ExprValue Rhs;
        uint8_t Next = (Op == SCRIPT_MATHOP_POW) ? Prec : (uint8_t)(Prec + 1);
        if (!ExprBin(Parser, Rhs, Next)) return false;
        if (!ExprApply(Out, Rhs, Op)) return false;
    }
    return true;
}

#endif // USE_SCRIPTS
