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
    // pinB is optional: -1 keeps the driver single-strip (Send only).
    LEDDriver(int pinA, int pinB = -1)
        : PinMaskA(1ULL << pinA), PinMaskB(pinB >= 0 ? (1ULL << pinB) : 0) {}

    // Configures the WS2812 data pin(s) as outputs (same config as the reference
    // working driver: pull-up off, pull-down on, interrupts disabled).
    void Setup()
    {
        gpio_config_t io_conf = {
            .pin_bit_mask = (uint64_t)PinMaskA | PinMaskB,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // TEMP DEBUG: raw GRB byte-stream bit-bang - a verbatim port of the reference
    // working driver (datasheet/LED.h Show()), bypassing the renderer. Used to verify
    // the WS2812 data line + timing on the hardware.
    void IRAM_ATTR Pulse(uint8_t *grb, uint16_t length, int pin)
    {
        uint32_t mask = 1ULL << pin;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

        portENTER_CRITICAL(&mux);

        uint32_t byteLength = (uint32_t)length * 3;
        uint32_t i = 0;
        while (byteLength--)
        {
            uint8_t channel = grb[i++];

            for (int8_t b = 7; b >= 0; b--)
            {
                if (channel & (1 << b))
                {
                    // T1H
                    GPIO.out_w1ts.val = mask; // High
                    NOP64();
                    NOP64();
                    NOP16();
                    GPIO.out_w1tc.val = mask; // Low
                    NOP64();
                }
                else
                {
                    // T0H
                    GPIO.out_w1ts.val = mask; // High
                    NOP16();
                    NOP16();
                    NOP4();
                    GPIO.out_w1tc.val = mask; // Low
                    NOP64();
                    NOP64();
                    NOP16();
                }
            }
        }

        portEXIT_CRITICAL(&mux);
        esp_rom_delay_us(80); // Reset Latch
    }

    // Sends `length` pixels to a single WS2812 strip (primary pin) with bit-banged
    // timing (the reference working driver's timing); reorders channels to GRB and
    // finishes with a reset latch.
    void IRAM_ATTR Send(ColourClass *pixels, uint16_t length)
    {
        uint32_t mask = PinMaskA;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

        portENTER_CRITICAL(&mux);

        for (uint16_t i = 0; i < length; i++)
        {
            // Explicitly order GRB for WS2812 compatibility
            uint8_t data[3] = {pixels[i].G, pixels[i].R, pixels[i].B};

            for (uint8_t ch = 0; ch < 3; ch++)
            {
                uint8_t channel = data[ch];
                for (int8_t b = 7; b >= 0; b--)
                {
                    if (channel & (1 << b))
                    {
                        // T1H: High for ~800ns (NOP144)
                        GPIO.out_w1ts.val = mask;
                        NOP144();
                        GPIO.out_w1tc.val = mask;
                        // T1L: Low for ~450ns (NOP64)
                        NOP64();
                    }
                    else
                    {
                        // T0H: High for ~400ns (NOP36)
                        GPIO.out_w1ts.val = mask;
                        NOP36();
                        GPIO.out_w1tc.val = mask;
                        // T0L: Low for ~850ns (NOP144)
                        NOP144();
                    }
                }
            }
        }
        portEXIT_CRITICAL(&mux);
        esp_rom_delay_us(80); // Reset Latch
    }

    // Sends `length` pixels to BOTH strips in parallel (pins A and B). Each bit-slot
    // drives both data lines from `pixelsA`/`pixelsB`: both go high, then pins carrying a
    // "0" drop at T0H and pins carrying a "1" drop at T1H (see class comment). Reorders
    // channels to GRB and finishes with a shared reset latch.
    void IRAM_ATTR SendParallel(ColourClass *pixelsA, ColourClass *pixelsB, uint16_t length)
    {
        uint32_t mask = PinMaskA | PinMaskB;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

        portENTER_CRITICAL(&mux);

        for (uint16_t i = 0; i < length; i++)
        {
            // Explicitly order GRB for WS2812 compatibility
            uint8_t dataA[3] = {pixelsA[i].G, pixelsA[i].R, pixelsA[i].B};
            uint8_t dataB[3] = {pixelsB[i].G, pixelsB[i].R, pixelsB[i].B};

            for (uint8_t ch = 0; ch < 3; ch++)
            {
                uint8_t a = dataA[ch];
                uint8_t b = dataB[ch];
                for (int8_t bitpos = 7; bitpos >= 0; bitpos--)
                {
                    bool ba = (a >> bitpos) & 1;
                    bool bb = (b >> bitpos) & 1;

                    GPIO.out_w1ts.val = mask; // both data lines high
                    if (ba != bb)
                    {
                        // Mixed bits: "0"-pin drops at T0H (~400ns, NOP36), "1"-pin at
                        // T1H (~800ns from the start, NOP144), then both stay low for the
                        // shared tail (T1L ~450ns). Each pin sees its own T0/T1 waveform.
                        uint32_t drop0 = (ba ? 0 : PinMaskA) | (bb ? 0 : PinMaskB);
                        uint32_t drop1 = (ba ? PinMaskA : 0) | (bb ? PinMaskB : 0);
                        NOP36();
                        GPIO.out_w1tc.val = drop0;
                        NOP108();
                        GPIO.out_w1tc.val = drop1;
                        NOP64();
                    }
                    else if (ba)
                    {
                        // both "1": T1H high (NOP144), T1L low (NOP64)
                        NOP144();
                        GPIO.out_w1tc.val = mask;
                        NOP64();
                    }
                    else
                    {
                        // both "0": T0H high (NOP36), T0L low (NOP144)
                        NOP36();
                        GPIO.out_w1tc.val = mask;
                        NOP144();
                    }
                }
            }
        }
        portEXIT_CRITICAL(&mux);
        esp_rom_delay_us(80); // Reset Latch
    }
};