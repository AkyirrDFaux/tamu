// Host-side test for the DAS reset-cause byte (Devices/DAS_v0.1/ResetCause.h).
//
// The byte is what the core receives as a log code on every boot of a DAS node, so its bit
// order is a contract: if it silently renumbers, every stored reset record means something
// else. ResetCause.h is host-clean on purpose (no chip header) so this can pin it, and
// Main.h static_asserts the same constants against the SPL's RCC_*RSTF macros - here the
// register bit positions are mirrored independently (26..31, spelled as shifts) so a wrong
// shift in the header cannot agree with itself.
#include <cstdint>
#include <cstdio>

#include "Devices/DAS_v0.1/ResetCause.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = 0x%llX, want 0x%llX\n", __FILE__, __LINE__, \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

int main() {
    // The constants are the register bits themselves (ch32v00x.h RCC_*RSTF), spelled out.
    CHECK_EQ(DAS_RST_PIN, (uint32_t)1u << 26);
    CHECK_EQ(DAS_RST_POR, (uint32_t)1u << 27);
    CHECK_EQ(DAS_RST_SW, (uint32_t)1u << 28);
    CHECK_EQ(DAS_RST_IWDG, (uint32_t)1u << 29);
    CHECK_EQ(DAS_RST_WWDG, (uint32_t)1u << 30);
    CHECK_EQ(DAS_RST_LPWR, (uint32_t)1u << 31);

    // No flag set at all (a reset whose flags were already cleared) reports zero.
    CHECK_EQ(DasResetCauseFlags(0), 0);

    // Each flag alone lands on its own bit, in register order: bit 0 = bit 26.
    for (int i = 0; i < 6; i++) CHECK_EQ(DasResetCauseFlags((uint32_t)1u << (26 + i)), (uint32_t)1u << i);

    // The shapes a soak is expected to produce: POR alone (the supply dropped) and POR|PIN
    // (a cold start, where the reset pin is also released - to be confirmed on the rig, this
    // is what the flag map predicts, not a measurement).
    CHECK_EQ(DasResetCauseFlags(DAS_RST_POR | DAS_RST_PIN), 0x03);
    CHECK_EQ(DasResetCauseFlags(DAS_RST_POR), 0x02);
    CHECK_EQ(DasResetCauseFlags(DAS_RST_SW), 0x04);

    // All six set: the full byte, and nothing beyond it.
    CHECK_EQ(DasResetCauseFlags(0xFC000000u), 0x3F);

    // Neighbours in the same register must not leak into the cause byte: RMVF (bit 24),
    // LSION (bit 0) and LSIRDY (bit 1) are not reset causes.
    CHECK_EQ(DasResetCauseFlags(0x01000000u /* RMVF */), 0);
    CHECK_EQ(DasResetCauseFlags(0x00000003u /* LSION|LSIRDY */), 0);
    CHECK_EQ(DasResetCauseFlags(0x01000003u | DAS_RST_POR), 0x02);

    // The return type is one byte: it is packed into the log code as-is.
    CHECK_EQ(sizeof(DasResetCauseFlags(0)), (size_t)1);

    if (failures) {
        std::printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    std::printf("reset cause: %d checks passed\n", checks);
    return 0;
}
