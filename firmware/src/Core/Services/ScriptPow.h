#pragma once

// base ^ exp in Q16.16 fixed point (ScriptPow.h) - part of the script service.
//
// A free function rather than a ScriptService member so the host-side test
// (test/native/script_test.cpp) can exercise the real implementation without the device layer;
// the Pow operator in ScriptExpr.h calls it.
//
// Fast paths: exp == 0.5 -> sqrt, integer exp -> repeated multiply (negative -> reciprocal).
// Any other exponent uses its binary expansion: with exp = int + sum(2^-k) over the set fraction
// bits, x^exp = x^int * prod(x^(2^-k)), and x^(2^-k) is k nested square roots. Fixed point end
// to end (no log/exp tables); the chain is only deepened while a lower fraction bit still needs
// it.
//
// The domain check runs FIRST, ahead of both fast paths: Number's sqrt clamps a non-positive
// input to 0 (Number.h), so testing the fractional-negative case after the 0.5 fast path made
// (-4)^0.5 return 0 and report success (Issues.md E42).

#include "Core/Types/Number.h"

// Docs/Services/Script.md "Math op": Power has "no fractional power of a negative base". A
// Q16.16 exponent is fractional when any of its 16 fraction bits is set, sign included, so this
// one predicate covers 0.5, 1.5 and -0.5 alike; a negative integer exponent stays in the domain.
inline bool ScriptPowDomainOk(Number Base, Number Exp)
{
    return Base.Value >= 0 || (((uint32_t)Exp.Value & 0xFFFFu) == 0);
}

// False on a domain failure (Out untouched); true otherwise.
inline bool ScriptPow(Number Base, Number Exp, Number &Out)
{
    if (!ScriptPowDomainOk(Base, Exp)) return false;
    if (Exp.Value == (int32_t)(1 << 15)) { Out = sqrt(Base); return true; } // 0.5

    bool Neg = Exp.Value < 0;
    if (Neg) Exp = -Exp;
    int32_t Ip = Exp.Value >> 16;      // integer part
    int32_t Frac = Exp.Value & 0xFFFF; // Q16.16 fraction
    if (Ip > 64) return false;

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
