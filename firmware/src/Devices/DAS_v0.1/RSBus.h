// Sized to hold at least two full transmissions: a frame is 12 header + 116 payload = 128
// bytes, plus the 0xAA sync byte = 129 transmitted bytes; two = 258. The ring reserves one
// empty slot for full detection, so 259 is the minimum; 260 round it up (RAM is scarce).
#define BUFFER_SIZE 260
#define RS485_EN_PORT GPIOD
#define RS485_EN_PIN GPIO_Pin_4
// Circular Buffer structure
volatile uint8_t RxBuffer[BUFFER_SIZE];
volatile uint16_t Head = 0; // ISR writes here
volatile uint16_t Tail = 0; // Main reads here

#include "Core/Functions/Packet.h"
#include "Core/Functions/Bus.h"

extern "C" {
    void USART1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
    // USART1 interrupt handler: stores received bytes in the ring buffer and clears error flags.
    void USART1_IRQHandler(void) {
        // Test the RXNE *status* flag (USART_IT_RXNE is an interrupt-config constant
        // whose encoding also matches PE/NE/LBD bits and would admit error bytes).
        if (USART1->STATR & USART_FLAG_RXNE) {
            uint8_t Data = (uint8_t)USART1->DATAR;

            // Next position, wrapped by a compare instead of a modulo: the CH32V003 core has
            // no integer divide, so a modulo by 260 is a call into a software divide helper
            // (about 100 cycles of ISR latency per received byte), which the 1 MBaud
            // experiment cannot afford at 10 us per byte.
            uint16_t NextHead = (uint16_t)(Head + 1);
            if (NextHead >= BUFFER_SIZE) NextHead = 0;

            // Only write if buffer is not full
            if (NextHead != Tail) {
                RxBuffer[Head] = Data;
                Head = NextHead;
            }
        }

        // Clear Error Flags (ORE/FE/NE are cleared by reading STATR followed by DATAR;
        // they are rc_w0/read-to-clear, so writing 0s back is a no-op)
        if (USART1->STATR & (USART_FLAG_ORE | USART_FLAG_FE | USART_FLAG_NE)) {
            (void)USART1->DATAR;
        }
    }
}

// Function to check if data is available
bool UARTAvailable() {
    return (Head != Tail);
}

// Function to read one byte from the buffer (tail update guarded against ISR race)
uint8_t UARTReadByte() {
    __disable_irq();
    uint8_t Data = RxBuffer[Tail];
    uint16_t Next = (uint16_t)(Tail + 1);
    if (Next >= BUFFER_SIZE) Next = 0;
    Tail = Next;
    __enable_irq();
    return Data;
}

// Configures GPIO, USART1, and interrupts for half-duplex RS485 communication.
void SetupRS485()
{
    GPIO_InitTypeDef  GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1, ENABLE);

    /* USART1 TX-->D.5   RX-->D.6 */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    RCC_APB2PeriphResetCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_USART1, DISABLE);

    // USART1: 460800 8N1, TX+RX, no flow control, 16x oversampling. The SPL's USART_Init is
    // deliberately not used: it derives the baud divisor from RCC_GetClocksFreq, whose two
    // prescaler tables (APBAHBPrescTable + ADCPrescTable) are 36 B of the DAS's .data. The
    // registers it would write are written here instead, from the same inputs: the struct it is
    // given is zeroed apart from the baud rate and the mode, and USART1 was just reset above, so
    // its clear-mask read-modify-writes reduce to these stores - same register values, same bytes.
    // PCLK2 = HCLK = SYSCLK = 48 MHz (hsi+pll, HPRE and PPRE2 both Div1: System/ch32v00x
    // SetSysClockTo_48MHZ_HSI), so USARTDIV = 48e6/(16*460800) = 6.5104 -> mantissa 6, fraction 8.
    constexpr uint32_t RS485ClockHz = 48000000; // PCLK2, fixed by the build's clock configuration
    constexpr uint32_t RS485Baud = 460800u;
    // USARTDIV in sixteenths, rounded - the SPL's own 25x/100x arithmetic yields the same 104.
    constexpr uint16_t RS485Brr = (uint16_t)((RS485ClockHz + RS485Baud / 2) / RS485Baud);
    static_assert(RS485Brr == 0x68, "460800 baud at a 48 MHz PCLK2 must be BRR 0x68");
    USART1->CTLR1 = USART_Mode_Tx | USART_Mode_Rx; // 8 data bits, no parity (both are zero)
    USART1->CTLR2 = 0;                             // 1 stop bit
    USART1->CTLR3 = 0;                             // no hardware flow control
    USART1->BRR = RS485Brr;

    // 5. Enable Interrupts
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); 
    NVIC_SetPriority(USART1_IRQn, 0); 
    NVIC_EnableIRQ(USART1_IRQn);
    __enable_irq();

    USART_Cmd(USART1, ENABLE);
}

// Microsecond timestamp from the SysTick counter (32-bit math, no 64-bit division helper).
static uint32_t RS485Micros()
{
    const uint32_t CyclesPerUs = SystemCoreClock / 1000000;
    return SysTick->CNT / CyclesPerUs;
}

// Shared CSMA/CD silence wait (Core/Functions/Bus.h): drain the ring, count silence in
// SysTick microseconds, busy-wait (the DAS's tight loop keeps bus turnaround latency low).
static void RS485WaitForSilence(uint8_t Priority)
{
    RS485WaitForSilence(Priority,
        [](void) -> bool {
            if (!UARTAvailable()) return false;
            while (UARTAvailable())
                UARTReadByte();
            return true;
        },
        RS485Micros,
        [](void) {});
}

// Transmits a packet with CSMA/CD collision avoidance and verifies the echo; retries up to
// RS485_RETRIES times. Sends and verifies as a byte stream (no tx/rx staging buffers), so the
// DAS's small stack is not exhausted even when called from a service handler that already
// holds its own frame (the frame's CRC is set by PacketConstruct/Append before we get here).
bool SendAndVerifyPacket(const PacketFrame &Data) {
    size_t PacketSize = 12 + PayloadBytes(Data);    // frame: header + payload, max 128
    size_t TotalTxSize = 1 + PacketSize;          // sync byte + frame (echo count)
    const uint8_t *Bytes = (const uint8_t *)&Data;

    for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485WaitForSilence(Data.Priority);

        PinHigh(LEDW);
        // Prepare Bus: Enable TX
        GPIO_WriteBit(RS485_EN_PORT, RS485_EN_PIN, Bit_SET);

        // Transmit
        USART_SendData(USART1, 0xAA);
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        for (size_t i = 0; i < PacketSize; i++) {
            USART_SendData(USART1, Bytes[i]);
            while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        }
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);

        SleepMicro(1000);
        // Switch to RX Mode
        GPIO_WriteBit(RS485_EN_PORT, RS485_EN_PIN, Bit_RESET);
        PinLow(LEDW);

        // Verify the echo byte-by-byte against what we just sent.
        size_t Got = 0;
        bool Match = true;
        uint32_t StartTick = Now();
        while (Got < TotalTxSize && (Now() - StartTick) < 50) {
            if (UARTAvailable()) {
                uint8_t Byte = UARTReadByte();
                uint8_t Expect = (Got == 0) ? 0xAA : Bytes[Got - 1];
                if (Byte != Expect) Match = false;
                Got++;
            }
        }

        if (Got == TotalTxSize && Match) {
            DasErrorFlag = false; // a successful transmit clears the latched bus error
            return true; // Success!
        }

        // Collision or failed echo: random backoff before retry
        Sleep((RawRand() % 16) + 1);
    }
    DasErrorFlag = true; // the red LED indicates the bus error (priority over the LED block)
    PinLow(LEDW);
    return false;
}

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the DAS ring buffer.
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &Byte) -> bool {
        if (!UARTAvailable()) return false;
        Byte = UARTReadByte();
        return true;
    });
}