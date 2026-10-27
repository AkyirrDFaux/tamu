// Host-side test for the Raw Resistance unit transform - Sensor Type 3 of the ResistiveMeasure
// block, field 3 "Measured Value" (Core/Devices/MeasuringResistance.h, called by both devices'
// MeasuringRun.h; card t_156125fc, Issues.md E54).
//
// Nothing in the tree exercised a unit transform before this file: meas_rail_test.cpp pins the
// two rail flags, so the transforms themselves were only ever checked by eye. The transform that
// was wrong is the one that MULTIPLIED before it divided: Rref * raw / (1023 - raw) overflows
// Q16.16 as soon as the product passes 32,767.999, i.e. from raw 100 counts up on the 330 kOhm
// reference, and FixedMul32 keeps the middle 32 bits instead of saturating - so the published
// value wrapped, usually into a negative number, with no flag and no log.
//
// Two things are pinned here, over the whole usable window (raw 1..1022, the counts MeasClampAdc
// allows) against exact double arithmetic for all three references of the DAS divider chain:
//
//   1. Every published value is within Q16.16 quantisation of the truth - one LSB of the divider
//      ratio (which is quantised before it is scaled by the reference) plus a few output LSBs.
//   2. Where the truth cannot be represented at all (above the format's 32,767.999 kOhm ceiling)
//      the transform SATURATES at N(32767) rather than wrapping. That is 10 counts on the 330 kOhm
//      reference (raw 1013..1022); it is the documented ceiling semantics, not an accident.
//
// Built in the two Number shapes the targets select - the DAS's 32-bit one (NUMBER_ONLY_32BIT,
// FixedMul32/FixedDiv32) and the core's 64-bit one. Both are wrong in the product-first form, so
// both are pinned here.
//
// No device dependency: built like align/stride/measrail (see run.sh).

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "Core/Devices/Measuring.h" // MeasClampAdc and the block, as the two devices see it

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

// The published value as kOhm, the way a reader sees the Number.
static double Kohm(Number Value) { return (double)Value.Value / 65536.0; }

static const Number AdcRes = N(1023);

// The DAS divider chain (MeasRangePin): 0 = 330 Ohm, 1 = 10 kOhm, 2 = 330 kOhm. The Valu has one
// fixed 10 kOhm reference (VALU_MEAS_REF_KOHM), which is the middle row - the transform is the
// same function on both devices, so that row covers it.
struct Reference { const char *Name; Number Rref; };

static const Reference Refs[] = {
    {"330 Ohm ", N(0.33)},
    {"10 kOhm ", N(10.0)},
    {"330 kOhm", N(330.0)},
};

// Q16.16 quantisation budget: the ratio is divided out to 1/65536 of a count-ratio and then
// scaled by the reference (that is one LSB of the ratio times Rref), and the final multiply
// truncates once more. Measured worst deviations on the real arithmetic: 2.0e-5 / 1.5e-4 / 5.0e-3
// kOhm for the three references - all inside this budget, while the wrap this test exists to catch
// is off by tens of kOhm.
static double Tolerance(Number Rref) { return Kohm(Rref) / 65536.0 + 6.0 / 65536.0; }

// The product-first form, kept only as the sensitivity witness below (what E54 found). It is not
// used for any expected value.
static Number ProductFirst(Number Sample, Number Rref) { return Rref * Sample / (AdcRes - Sample); }

int main() {
    // ---- 1. The whole usable window, every reference.
    for (const Reference &Ref : Refs) {
        const double Tol = Tolerance(Ref.Rref);
        int OutOfBand = 0;      // published value outside the quantisation budget
        int Saturated = 0;      // published the ceiling
        int SignFlipped = 0;    // truth positive, published negative - the E54 failure shape
        int NonMonotonic = 0;   // the divider is monotonic in the raw count, so the output is too
        int FirstSaturated = 0; // first count that cannot be represented
        double Worst = 0;
        int WorstRaw = 0;
        double Previous = -1;

        for (int Raw = 1; Raw <= 1022; Raw++) {
            const Number Sample = MeasClampAdc(Number(Raw));
            const Number Got = MeasRawResistanceKohm(Sample, Ref.Rref);
            const double Exact = Kohm(Ref.Rref) * Raw / (1023.0 - Raw);

            if (Exact > 32767.0) {
                // Unrepresentable: the contract is saturation at the ceiling, never a wrap.
                Saturated++;
                if (!FirstSaturated) FirstSaturated = Raw;
                if (Got.Value != N(32767).Value) {
                    OutOfBand++;
                    std::printf("FAIL %s raw %d: %f kOhm is above the format ceiling, want the "
                                "N(32767) clamp, got %.3f\n",
                                Ref.Name, Raw, Exact, Kohm(Got));
                }
            } else {
                const double Error = std::fabs(Kohm(Got) - Exact);
                if (Error > Tolerance(Ref.Rref)) {
                    OutOfBand++;
                    std::printf("FAIL %s raw %d: published %.4f kOhm, exact %.4f (error %.6f > %.6f)\n",
                                Ref.Name, Raw, Kohm(Got), Exact, Error, Tol);
                }
                if (Error > Worst) { Worst = Error; WorstRaw = Raw; }
            }

            if (Exact >= 0 && Kohm(Got) < 0) {
                SignFlipped++;
                std::printf("FAIL %s raw %d: published %.4f kOhm for a positive resistance\n",
                            Ref.Name, Raw, Kohm(Got));
            }
            if (Kohm(Got) < Previous) {
                NonMonotonic++;
                std::printf("FAIL %s raw %d: published %.4f kOhm, below the previous count's %.4f\n",
                            Ref.Name, Raw, Kohm(Got), Previous);
            }
            Previous = Kohm(Got);
        }

        checks += 4;
        if (OutOfBand || SignFlipped || NonMonotonic) failures++;
        std::printf("%s: 1022 counts, worst deviation %.6f kOhm @raw %d, %d saturating, first %d\n",
                    Ref.Name, Worst, WorstRaw, Saturated, FirstSaturated);

        // The 10 kOhm and 330 Ohm references reach the ceiling nowhere: 10 * 1022 = 10,220 kOhm
        // and 0.33 * 1022 = 337 kOhm both fit comfortably.
        if (Ref.Rref.Value != N(330.0).Value) {
            CHECK(Saturated == 0);
        }
        // "330 kOhm saturates the last 10 usable counts" is the count frame E54 and this fix are
        // stated in - raw 1012 publishes 30,360 kOhm (representable), raw 1013 is 33,429 kOhm.
        else {
            CHECK(Saturated == 10);
            CHECK(FirstSaturated == 1013);
        }
    }

    // ---- 2. The values in the finding, so the frame cannot drift (raw count -> kOhm).
    // 330 kOhm, the range the defect lived on.
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(97)), N(330.0))) - 34.568) < 0.01);
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(99)), N(330.0))) - 35.357) < 0.01);
    // First broken count of the product-first form: 35.753 kOhm, published as -35.25 before.
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(100)), N(330.0))) - 35.753) < 0.01);
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(500)), N(330.0))) - 315.488) < 0.02);
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(1012)), N(330.0))) - 30360.0) < 1.0);
    // The sensor E54 names as the first wrong one: a 74 kOhm resistor sits at raw 187 on this range
    // (the count is coarse up here) and used to publish -4.577 kOhm; it is 73.816 kOhm now.
    CHECK(std::fabs(Kohm(MeasRawResistanceKohm(MeasClampAdc(Number(187)), N(330.0))) - 73.816) < 0.02);

    // ---- 3. The two rail counts at the ceiling. A railed input dithers between the top of the
    // usable window (1022) and full scale (1023); both clamp to 1022, both sit above the format
    // ceiling on the 330 kOhm reference, and both must publish the ceiling - the same two counts
    // the Open Circuit flag is judged on (Core/Devices/MeasuringFlags.h).
    for (int Raw : {1022, 1023}) {
        const Number Sample = MeasClampAdc(Number(Raw));
        CHECK(MeasRawResistanceKohm(Sample, N(330.0)).Value == N(32767).Value);
        CHECK(MeasOpenCircuit(2, Sample) == true);
    }
    // The bottom rail behaves the other way: raw 0 clamps to 1, so the 330 Ohm reference publishes
    // 0.33/1022 = 0.000323 kOhm rather than dividing by zero, and that is the Short Circuit rail.
    CHECK(MeasRawResistanceKohm(MeasClampAdc(Number(0)), N(0.33)).Value == N(0.33).Value / 1022);
    CHECK(MeasShortCircuit(0, MeasClampAdc(Number(0))) == true);

    // ---- 4. The ceiling is a clamp in the format's own units, not an arbitrary number: N(32767)
    // is the largest Q16.16 integer below 32,767.999, and no count on any reference publishes the
    // INT32_MIN/INT32_MAX edges a wrap leaves behind.
    for (const Reference &Ref : Refs) {
        for (int Raw = 1; Raw <= 1022; Raw++) {
            const Number Got = MeasRawResistanceKohm(MeasClampAdc(Number(Raw)), Ref.Rref);
            CHECK(Got.Value >= 0);
            CHECK(Got.Value <= N(32767).Value);
        }
    }

    // ---- 5. Sensitivity witness: the product-first form IS wrong at these counts in both Number
    // shapes (that is why this file exists), so a regression back to it cannot pass silently.
    CHECK(ProductFirst(MeasClampAdc(Number(100)), N(330.0)).Value < 0);
    CHECK(std::fabs(Kohm(ProductFirst(MeasClampAdc(Number(500)), N(330.0))) - 315.488) > 100.0);

    if (failures) {
        std::printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    std::printf("meas raw resistance transform: %d checks passed\n", checks);
    return 0;
}
