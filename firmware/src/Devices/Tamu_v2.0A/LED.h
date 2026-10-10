#include "driver/gpio.h"
#include "esp_attr.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#define NOP4() __asm__ volatile("nop; nop; nop; nop;")
#define NOP8() \
    NOP4();    \
    NOP4();
#define NOP16() \
    NOP4();     \
    NOP4();     \
    NOP4();     \
    NOP4();
#define NOP32() \
    NOP16();    \
    NOP16();
#define NOP36() \
    NOP16();    \
    NOP16();    \
    NOP4();
#define NOP64() \
    NOP16();    \
    NOP16();    \
    NOP16();    \
    NOP16();
#define NOP108() \
    NOP64();     \
    NOP32();     \
    NOP8();      \
    NOP4();
#define NOP144() \
    NOP64();     \
    NOP64();     \
    NOP16();

// Bit-banged WS2812 driver (reference timing from the working datasheet LED.h: a
// continuous bit-bang, no inter-bit gaps, reset latch at the end). Sends up to two
// strips (pins 0, 3) IN PARALLEL: the data lines share one timeline, so both chains
// update in the wire time of a single chain. Per bit-slot both pins start high; pins
// carrying a "0" drop low at T0H (~0.4us) and pins carrying a "1" drop at T1H (~0.8us),
// so each pin sees its own T0/T1 waveform.
class LEDDriver
{
private:
    uint32_t PinMaskA; // primary strip
    uint32_t PinMaskB; // secondary strip (0 when unused)

public:
    // pinB is optional: -1 keeps the driver single-strip (SendParallel with an empty mask).
    LEDDriver(int pinA, int pinB = -1)
        : PinMaskA(1ULL << pinA), PinMaskB(pinB >= 0 ? (1ULL << pinB) : 0) {}

    // Configures the WS2812 data pin(s) as outputs (same config as the reference
    // working driver: pull-up off, pull-down on, interrupts disabled).
    void Setup()
    {
        gpio_config_t IoConf = {
            .pin_bit_mask = (uint64_t)PinMaskA | PinMaskB,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&IoConf);
    }

    // Sends `length` pixels to BOTH strips in parallel (pins A and B). Each bit-slot
    // drives both data lines from `pixelsA`/`pixelsB`: both go high, then pins carrying a
    // "0" drop at T0H and pins carrying a "1" drop at T1H (see class comment). Reorders
    // channels to GRB and finishes with a shared reset latch.
    void IRAM_ATTR SendParallel(ColourClass *pixelsA, ColourClass *pixelsB, uint16_t Length)
    {
        uint32_t Mask = PinMaskA | PinMaskB;
        static portMUX_TYPE Mux = portMUX_INITIALIZER_UNLOCKED;

        portENTER_CRITICAL(&Mux);

        for (uint16_t I = 0; I < Length; I++)
        {
            // Explicitly order GRB for WS2812 compatibility
            uint8_t dataA[3] = {pixelsA[I].G, pixelsA[I].R, pixelsA[I].B};
            uint8_t dataB[3] = {pixelsB[I].G, pixelsB[I].R, pixelsB[I].B};

            for (uint8_t Ch = 0; Ch < 3; Ch++)
            {
                uint8_t A = dataA[Ch];
                uint8_t B = dataB[Ch];
                for (int8_t Bitpos = 7; Bitpos >= 0; Bitpos--)
                {
                    bool Ba = (A >> Bitpos) & 1;
                    bool Bb = (B >> Bitpos) & 1;

                    GPIO.out_w1ts.val = Mask; // both data lines high
                    if (Ba != Bb)
                    {
                        // Mixed bits: "0"-pin drops at T0H (~400ns, NOP36), "1"-pin at
                        // T1H (~800ns from the start, NOP144), then both stay low for the
                        // shared tail (T1L ~450ns). Each pin sees its own T0/T1 waveform.
                        uint32_t Drop0 = (Ba ? 0 : PinMaskA) | (Bb ? 0 : PinMaskB);
                        uint32_t Drop1 = (Ba ? PinMaskA : 0) | (Bb ? PinMaskB : 0);
                        NOP36();
                        GPIO.out_w1tc.val = Drop0;
                        NOP108();
                        GPIO.out_w1tc.val = Drop1;
                        NOP64();
                    }
                    else if (Ba)
                    {
                        // both "1": T1H high (NOP144), T1L low (NOP64)
                        NOP144();
                        GPIO.out_w1tc.val = Mask;
                        NOP64();
                    }
                    else
                    {
                        // both "0": T0H high (NOP36), T0L low (NOP144)
                        NOP36();
                        GPIO.out_w1tc.val = Mask;
                        NOP144();
                    }
                }
            }
        }
        portEXIT_CRITICAL(&Mux);
        esp_rom_delay_us(80); // Reset Latch
    }
};