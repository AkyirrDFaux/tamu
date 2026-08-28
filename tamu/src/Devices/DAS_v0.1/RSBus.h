// Sized to hold a full frame echo: max frame = 0xAA + 12 header + 292 payload = 305 bytes.
#define BUFFER_SIZE 320
#define RS485_EN_PORT GPIOD
#define RS485_EN_PIN GPIO_Pin_4
// Circular Buffer structure
volatile uint8_t rx_buffer[BUFFER_SIZE];
volatile uint16_t head = 0; // ISR writes here
volatile uint16_t tail = 0; // Main reads here

// True once SetupRS485() has enabled the transceiver (gates bus logging).
static bool g_rs485_ready = false;

#include "Core/Functions/Packet.h"
#include "Core/Functions/Bus.h"

extern "C" {
    void USART1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
    // USART1 interrupt handler: stores received bytes in the ring buffer and clears error flags.
    void USART1_IRQHandler(void) {
        // Test the RXNE *status* flag (USART_IT_RXNE is an interrupt-config constant
        // whose encoding also matches PE/NE/LBD bits and would admit error bytes).
        if (USART1->STATR & USART_FLAG_RXNE) {
            uint8_t data = (uint8_t)USART1->DATAR;

            // Calculate next position
            uint16_t next_head = (head + 1) % BUFFER_SIZE;

            // Only write if buffer is not full
            if (next_head != tail) {
                rx_buffer[head] = data;
                head = next_head;
            }
            // Optional: Handle overflow here if needed
        }

        // Clear Error Flags (ORE/FE/NE are cleared by reading STATR followed by DATAR;
        // they are rc_w0/read-to-clear, so writing 0s back is a no-op)
        if (USART1->STATR & (USART_FLAG_ORE | USART_FLAG_FE | USART_FLAG_NE)) {
            (void)USART1->DATAR;
        }
    }
}

// Function to check if data is available
bool UART_Available() {
    return (head != tail);
}

// Function to read one byte from the buffer
uint8_t UART_ReadByte() {
    uint8_t data = rx_buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return data;
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

    USART_InitTypeDef USART_InitStr = {0};
    USART_InitStr.USART_BaudRate = 115200;
    USART_InitStr.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &USART_InitStr);

    // 5. Enable Interrupts
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); 
    NVIC_SetPriority(USART1_IRQn, 0); 
    NVIC_EnableIRQ(USART1_IRQn);
    __enable_irq();

    USART_Cmd(USART1, ENABLE);
    g_rs485_ready = true;
}

#define RS485_RETRIES 3

// CSMA/CD timings at 115200 baud (1 byte = ~86.8us)
#define RS485_BYTE_TIME_US     87
#define RS485_SILENCE_BYTES    8

// Microsecond timestamp from the SysTick counter (32-bit math, no 64-bit division helper).
static uint32_t RS485_Micros()
{
    return SysTick->CNT / (SystemCoreClock / 1000000);
}

// Wait for the line to be silent for 8 bytes + a random 0-7 byte backoff.
// Bounded to RS485_SILENCE_TIMEOUT_MS so a continuously-busy or shorted bus
// cannot wedge the caller forever.
#define RS485_SILENCE_TIMEOUT_MS 100

static void RS485_WaitForSilence()
{
    uint32_t silence_us = (RS485_SILENCE_BYTES + (RawRand() % 8)) * RS485_BYTE_TIME_US;
    uint32_t idle_since = RS485_Micros();
    uint32_t wait_start = Now();

    for (;;)
    {
        if (UART_Available())
        {
            // Bus activity: drain and restart the silence window
            while (UART_Available())
                UART_ReadByte();
            idle_since = RS485_Micros();
        }
        if ((RS485_Micros() - idle_since) >= silence_us)
            return;
        if ((Now() - wait_start) >= RS485_SILENCE_TIMEOUT_MS)
            return; // Give up: transmit into the best window we had
    }
}

// Transmits a packet with CSMA/CD collision avoidance and verifies the echo; retries up to
// RS485_RETRIES times. Sends and verifies as a byte stream (no tx/rx staging buffers), so the
// DAS's small stack is not exhausted even when called from a service handler that already
// holds its own frame (the frame's CRC is set by PacketConstruct/Append before we get here).
bool SendAndVerifyPacket(const PacketFrame &Data) {
    size_t packet_size = 12 + PayloadBytes(Data);       // Header + payload (packed struct)
    size_t total_tx_size = 1 + packet_size;           // Start byte (0xAA) + packet
    const uint8_t *bytes = (const uint8_t *)&Data;

    for (int attempt = 0; attempt < RS485_RETRIES; attempt++) {
        // CSMA/CD: only transmit once the line has been silent
        RS485_WaitForSilence();

        PinHigh(LEDW);
        // Prepare Bus: Enable TX
        GPIO_WriteBit(RS485_EN_PORT, RS485_EN_PIN, Bit_SET);

        // Transmit
        USART_SendData(USART1, 0xAA);
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        for (size_t i = 0; i < packet_size; i++) {
            USART_SendData(USART1, bytes[i]);
            while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        }
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);

        SleepMicro(1000);
        // Switch to RX Mode
        GPIO_WriteBit(RS485_EN_PORT, RS485_EN_PIN, Bit_RESET);
        PinLow(LEDW);

        // Verify the echo byte-by-byte against what we just sent.
        size_t got = 0;
        bool match = true;
        uint32_t start_tick = Now();
        while (got < total_tx_size && (Now() - start_tick) < 50) {
            if (UART_Available()) {
                uint8_t b = UART_ReadByte();
                uint8_t expect = (got == 0) ? 0xAA : bytes[got - 1];
                if (b != expect) match = false;
                got++;
            }
        }

        if (got == total_tx_size && match) {
            return true; // Success!
        }

        // Collision or failed echo: random backoff before retry
        Sleep((RawRand() % 16) + 1);
    }
    PinLow(LEDW);
    return false;
}

// Receives and validates a full PacketFrame using the shared bus assembler
// (Core/Functions/Bus.h); the byte source pulls from the DAS ring buffer.
int ReceivePacket(PacketFrame *Data)
{
    return ReceivePacketFrame(Data, [](uint8_t &b) -> bool {
        if (!UART_Available()) return false;
        b = UART_ReadByte();
        return true;
    });
}