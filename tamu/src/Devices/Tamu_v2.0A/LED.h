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

    // Interrupt-masked section length in bits and the gap between chunks. A full 86-pixel
    // strip sent with interrupts masked for ~3ms lets the RS-485 UART RX FIFO (128 B)
    // overflow and drops large frames, so the bit-bang yields for LED_GAP_US every
    // LED_CHUNK_BITS bits - a low period far under the WS2812 reset threshold (~50us) that
    // lets the UART ISR drain. 48 bits ~= 57us ~= 0.7 bytes of FIFO per chunk.
    static const uint16_t LED_CHUNK_BITS = 48;
    static const uint16_t LED_GAP_US = 15;

public:
    // Only store pin info
    LEDDriver(int pin_number) : PinMask(1ULL << pin_number) {}

    // Configures the WS2812 data pin as an output.
    void Setup()
    {
        gpio_reset_pin((gpio_num_t)__builtin_ctz(PinMask));
        gpio_set_direction((gpio_num_t)__builtin_ctz(PinMask), GPIO_MODE_OUTPUT);
    }

    // Sends `length` pixels to the WS2812 strip with bit-banged timing (interrupts disabled
    // only for short chunks); reorders channels to GRB and finishes with a reset latch.
    void IRAM_ATTR Send(ColourClass *pixels, uint16_t length)
    {
        uint32_t mask = PinMask;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

        portENTER_CRITICAL(&mux);

        uint16_t bit = 0;
        for (uint16_t i = 0; i < length; i++)
        {
            // Explicitly order GRB for WS2812 compatibility
            uint8_t data[3] = {pixels[i].G, pixels[i].R, pixels[i].B};

            for (uint8_t ch = 0; ch < 3; ch++)
            {
                uint8_t channel = data[ch];
                for (int8_t b = 7; b >= 0; b--)
                {
                    // Yield every LED_CHUNK_BITS bits: drop the critical section while the
                    // line sits low (well under the reset threshold) so the RS-485 UART RX
                    // ISR can drain its FIFO.
                    if ((bit & (LED_CHUNK_BITS - 1)) == 0)
                    {
                        portEXIT_CRITICAL(&mux);
                        esp_rom_delay_us(LED_GAP_US);
                        portENTER_CRITICAL(&mux);
                    }
                    bit++;

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