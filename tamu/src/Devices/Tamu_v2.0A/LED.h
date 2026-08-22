#include "driver/gpio.h"
#include "esp_attr.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#define NOP4() __asm__ volatile("nop; nop; nop; nop;")
#define NOP16() \
    NOP4();     \
    NOP4();     \
    NOP4();     \
    NOP4();
#define NOP32() \
    NOP16();    \
    NOP16();
#define NOP64() \
    NOP16();    \
    NOP16();    \
    NOP16();    \
    NOP16();

class LEDDriver
{
private:
    uint32_t PinMask;

public:
    // Only store pin info
    LEDDriver(int pin_number) : PinMask(1ULL << pin_number) {}

    // Configures the WS2812 data pin as an output.
    void Setup()
    {
        gpio_reset_pin((gpio_num_t)__builtin_ctz(PinMask));
        gpio_set_direction((gpio_num_t)__builtin_ctz(PinMask), GPIO_MODE_OUTPUT);
    }

    // Sends `length` pixels to the WS2812 strip with bit-banged timing (interrupts disabled); reorders channels to GRB and finishes with a reset latch.
    void IRAM_ATTR Send(ColourClass *pixels, uint16_t length)
    {
        uint32_t mask = PinMask;
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
                        NOP64();
                        NOP64();
                        NOP16();
                        GPIO.out_w1tc.val = mask;
                        // T1L: Low for ~450ns (NOP64)
                        NOP64();
                    }
                    else
                    {
                        // T0H: High for ~400ns (NOP36)
                        GPIO.out_w1ts.val = mask;
                        NOP16();
                        NOP16();
                        NOP4();
                        GPIO.out_w1tc.val = mask;
                        // T0L: Low for ~850ns (NOP144)
                        NOP64();
                        NOP64();
                        NOP16();
                    }
                }
            }
        }
        portEXIT_CRITICAL(&mux);
        esp_rom_delay_us(80); // Reset Latch
    }
};