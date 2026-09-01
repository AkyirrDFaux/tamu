// DAS v0.1 Standalone Bootloader — bare-metal, zero HAL.
//
// ~500 bytes flash target. Direct register access for all peripherals.
// Protocol (Docs/Services/Bootloader.md):
//   CID 0 — Bootloader Check:  SN -> SN + bool (true=in bootloader)
//   CID 1 — App Write:         FRAG stream of app binary chunks
//
// Entry: button (PC0) held at power-on/reset, or jump from app.
// Exit:  manual reset (without button held).

#ifdef BOOTLOADER_BINARY

#include <cstdint>
#include <cstring>

// === Register definitions ===

// RCC
#define RCC_APB2PCENR   (*(volatile uint32_t *)0x40021018)
#define RCC_EN_GPIOA    (1u << 2)
#define RCC_EN_GPIOC    (1u << 4)
#define RCC_EN_GPIOD    (1u << 5)
#define RCC_EN_USART1   (1u << 14)

// GPIO
struct GPIO {
    volatile uint32_t CFGLR, RESERVED0, INDR, OUTDR, BSHR, BCR, LCKR;
};
#define GPIOA ((GPIO *)0x40010800)
#define GPIOC ((GPIO *)0x40011000)
#define GPIOD ((GPIO *)0x40011400)

// USART
struct USART {
    volatile uint16_t STATR; uint16_t R0;
    volatile uint16_t DATAR; uint16_t R1;
    volatile uint16_t BRR;   uint16_t R2;
    volatile uint16_t CTLR1; uint16_t R3;
    volatile uint16_t CTLR2; uint16_t R4;
    volatile uint16_t CTLR3; uint16_t R5;
    volatile uint16_t GPR;   uint16_t R6;
};
#define USART1 ((USART *)0x40013800)
#define USART_TXE  (1u << 7)
#define USART_TC   (1u << 6)
#define USART_RXNE (1u << 5)

// FLASH
struct FLASH_r {
    volatile uint32_t ACTLR, KEYR, OBKEYR, STATR, CTLR, ADDR, RESERVED, OBR, WPR, MODEKEYR, BOOT_MODEKEYR;
};
#define FLASHr ((FLASH_r *)0x40022000)
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu
#define FLASH_BSY      (1u << 0)
#define FLASH_EOP      (1u << 5)
#define FLASH_PG       (1u << 0)
#define FLASH_STRT     (1u << 6)
#define FLASH_LOCK_BIT (1u << 7)
#define FLASH_PAGE_ER  (1u << 17)
#define FLASH_PAGE_PG  (1u << 16)

// SysTick (WCH custom at 0xE000F000)
struct STK {
    volatile uint32_t CTLR, SR, CNT, RESERVED, CMP;
};
#define SYSTICK ((STK *)0xE000F000)

// === Constants ===

#define APP_START     0x0600u
#define STORAGE_START 0x3800u
#define APP_MAX_SIZE  (STORAGE_START - APP_START)
#define FLASH_CTRL    0x08000000u

#define BTN_PORT   GPIOC
#define BTN_PIN    0  // PC0
#define LEDR_PORT  GPIOA
#define LEDR_PIN   1  // PA1
#define RS485_PORT GPIOD
#define RS485_PIN  4  // PD4

#define ADDR_BROADCAST 0xFFFF

// Packet flags (must match Core/Functions/Packet.h)
#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3)
#define FLAG_FRAG   (1 << 4)

// === Timing (SysTick free-running at 48 MHz) ===

static inline uint32_t tick() { return SYSTICK->CNT; }

// === LED ===

static void led_on()  { GPIOA->OUTDR |= (1u << LEDR_PIN); }
static void led_off() { GPIOA->OUTDR &= ~(1u << LEDR_PIN); }

// 80% duty cycle blink (~5 Hz). Call frequently (e.g. from UART polling loops).
// ON for 160 ms, OFF for 40 ms (200 ms period).
static void led_update(void)
{
    static uint32_t last = 0;
    static bool on = true;
    uint32_t now = tick();
    uint32_t elapsed = (uint32_t)(now - last);
    if (on && elapsed >= 3840000u) {       // 160 ms at 24 MHz
        led_off();
        on = false;
        last = now;
    } else if (!on && elapsed >= 960000u) { // 40 ms at 24 MHz
        led_on();
        on = true;
        last = now;
    }
}

// === UART ===

static void uart_init(void)
{
    USART1->BRR = 208;  // 24 MHz HSI / 115200
    USART1->CTLR1 = (1u << 13) | (1u << 3) | (1u << 2);  // UE | TE | RE
}

static void uart_tx_enable(void)  { GPIOD->OUTDR |= (1u << RS485_PIN); }
static void uart_tx_disable(void)
{
    while (!(USART1->STATR & USART_TC)) ;
    GPIOD->OUTDR &= ~(1u << RS485_PIN);
}

static void uart_send_byte(uint8_t b)
{
    while (!(USART1->STATR & USART_TXE)) ;
    USART1->DATAR = b;
}

static void uart_send(const uint8_t *data, uint16_t len)
{
    uart_tx_enable();
    for (uint16_t i = 0; i < len; i++) uart_send_byte(data[i]);
    uart_tx_disable();
}

static int uart_recv_byte(uint32_t timeout_ms)
{
    uint32_t deadline = tick() + timeout_ms * 24000u;  // 24 MHz HSI
    while (!(USART1->STATR & USART_RXNE)) {
        if ((int32_t)(tick() - deadline) >= 0) return -1;
    }
    return (int)(uint8_t)USART1->DATAR;
}

// === Flash ===

static void flash_unlock(void)
{
    FLASHr->KEYR = FLASH_KEY1;
    FLASHr->KEYR = FLASH_KEY2;
    FLASHr->MODEKEYR = FLASH_KEY1;
    FLASHr->MODEKEYR = FLASH_KEY2;
}

static void flash_lock(void)
{
    FLASHr->CTLR |= FLASH_LOCK_BIT;
}

static bool flash_erase_app(void)
{
    flash_unlock();
    for (uint32_t addr = FLASH_CTRL + APP_START; addr < FLASH_CTRL + STORAGE_START; addr += 64)
    {
        FLASHr->ADDR = addr;
        FLASHr->CTLR |= FLASH_PAGE_ER | FLASH_STRT;
        while (FLASHr->STATR & FLASH_BSY) ;
    }
    flash_lock();
    return true;
}

static bool flash_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (offset + len > APP_MAX_SIZE || len == 0) return false;

    uint32_t faddr = FLASH_CTRL + APP_START + offset;
    flash_unlock();

    // Write 4-byte words
    uint32_t start = faddr & ~3u;
    uint32_t end = (faddr + len + 3u) & ~3u;
    for (uint32_t a = start; a < end; a += 4)
    {
        uint32_t w = 0;
        for (int k = 0; k < 4; k++) {
            uint32_t byte_addr = a + k;
            if (byte_addr >= faddr && byte_addr < faddr + len)
                w |= ((uint32_t)data[byte_addr - faddr]) << (8 * k);
            else
                w |= ((uint32_t)(*(volatile uint8_t *)byte_addr)) << (8 * k);
        }

        FLASHr->CTLR |= FLASH_PG;
        *(volatile uint32_t *)a = w;
        while (FLASHr->STATR & FLASH_BSY) ;
        FLASHr->CTLR &= ~FLASH_PG;
    }

    flash_lock();
    return true;
}

// === Jump to application ===

static void jump_to_app(void)
{
    __asm volatile("csrw mtvec, %0" : : "r"((uint32_t)(APP_START | 3)));
    extern uint32_t _eusrstack;
    __asm volatile("mv sp, %0" : : "r"(&_eusrstack));
    uint32_t app_reset = *(volatile uint32_t *)(APP_START + 4);
    __asm volatile("csrw mepc, %0" : : "r"(app_reset));
    uint32_t mstatus = (3u << 11) | (1u << 7);
    __asm volatile("csrw mstatus, %0" : : "r"(mstatus));
    __asm volatile("mret");
    __builtin_unreachable();
}

// === Packet framing (RS-485, same wire format as DAS) ===
//
// Wire layout after 0xAA sync (12 bytes header + payload):
//   [0] CRC8  [1] Flags  [2] Priority  [3] PayloadLen (units of 4)
//   [4-5] idTarget  [6-7] idSource  [8-9] srvTarget  [10-11] srvSource
//   [12..] Payload

static int receive_packet(uint8_t *buf, uint32_t bufsize)
{
    while (1)
    {
        int b;
        do { b = uart_recv_byte(5); led_update(); } while (b < 0);
        if (b != 0xAA) continue;

        buf[0] = 0xAA;
        uint32_t pos = 1;
        uint16_t payload_bytes;
        uint8_t crc;

        // Header: 12 bytes after sync (CRC8 + 11 bytes)
        for (int i = 0; i < 12; i++) {
            b = uart_recv_byte(5);
            led_update();
            if (b < 0) goto resync;
            buf[pos++] = (uint8_t)b;
        }

        // buf[1]=CRC8, [2]=Flags, [3]=Priority, [4]=PayloadLen
        payload_bytes = (uint16_t)buf[4] * 4;
        if (payload_bytes > 276) continue;

        for (uint16_t i = 0; i < payload_bytes; i++) {
            b = uart_recv_byte(5);
            led_update();
            if (b < 0) goto resync;
            buf[pos++] = (uint8_t)b;
        }

        // CRC8: covers bytes [2]..end (Flags through Payload), compare to [1]
        crc = 0;
        for (uint32_t i = 2; i < pos; i++) {
            uint8_t c = crc ^ buf[i];
            for (int j = 0; j < 8; j++)
                c = (c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1);
            crc = c;
        }
        if (crc == buf[1]) return (int)pos;
        continue;

resync:;
    }
}

static void send_response(const uint8_t *req, const uint8_t *payload, uint8_t len)
{
    // Build response frame (without sync byte). Swap src/tgt per standard protocol.
    // req[] layout: [0]=0xAA, [1]=CRC8, [2]=Flags, [3]=Priority, [4]=PayloadLen,
    //   [5-6]=idTarget, [7-8]=idSource, [9-10]=srvTarget, [11-12]=srvSource
    static uint8_t frame[300];
    frame[0] = 0;                                    // CRC placeholder
    frame[1] = req[2] | 0x08;                        // flags = request flags | TYPE
    frame[2] = 128;                                   // priority
    uint8_t padded = (len + 3) & ~3;
    frame[3] = padded / 4;                            // payload_len
    frame[4] = req[7]; frame[5] = req[8];            // id_tgt = request idSource
    frame[6] = 0x01; frame[7] = 0x00;               // id_src = 1 (core)
    frame[8] = req[11]; frame[9] = req[12];          // srv_tgt = request srvSource
    frame[10] = req[9]; frame[11] = req[10];         // srv_src = request srvTarget
    memcpy(frame + 12, payload, len);
    for (uint8_t i = len; i < padded; i++) frame[12 + i] = 0;

    uint8_t crc = 0;
    for (uint32_t i = 1; i < (uint32_t)(12 + padded); i++) {
        uint8_t c = crc ^ frame[i];
        for (int j = 0; j < 8; j++) c = (c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1);
        crc = c;
    }
    frame[0] = crc;

    static uint8_t wire[302];
    wire[0] = 0xAA;
    memcpy(wire + 1, frame, 12 + padded);
    uart_send(wire, 1 + 12 + padded);
}

// === Entry point ===

extern "C" void handle_reset(void)
{
    // Minimal startup: just set stack pointer, then jump to main.
    // BSS/data init, SystemInit, __libc_init_array all skipped.
    asm volatile("la sp, _eusrstack");
    asm volatile("j main");
}

int main(void)
{
    // Enable peripheral clocks
    RCC_APB2PCENR |= RCC_EN_GPIOA | RCC_EN_GPIOC | RCC_EN_GPIOD | RCC_EN_USART1;

    // LED: PA1 push-pull output
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xF << 4)) | (0x3 << 4);
    led_off();

    // Button: PC0 input pull-up (mode 0x8 + OUTDR=1 selects pull-up)
    GPIOC->CFGLR = (GPIOC->CFGLR & ~(0xF << 0)) | (0x8 << 0);
    GPIOC->OUTDR |= (1u << BTN_PIN);

    // UART TX: PD5 AF push-pull
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 20)) | (0xB << 20);
    // UART RX: PD6 input pull-up
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 24)) | (0x8 << 24);
    // RS485 direction: PD4 push-pull output (default low = RX)
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 16)) | (0x3 << 16);
    GPIOD->OUTDR &= ~(1u << RS485_PIN);

    // SysTick free-running counter
    SYSTICK->CTLR = (1u << 0) | (1u << 2) | (1u << 8);  // Enable | HCLK | RELOAD

    // Check boot button: if NOT held, jump to app
    if (GPIOC->INDR & (1u << BTN_PIN))
        jump_to_app();

    // Bootloader mode
    uart_init();
    led_on();  // start 80% duty cycle

    static uint8_t pkt[300];
    uint32_t app_offset = 0;
    bool first_frag = true;

    while (1)
    {
        int len = receive_packet(pkt, sizeof(pkt));
        if (len <= 0) continue;

        uint16_t id_tgt  = pkt[5] | (pkt[6] << 8);
        uint16_t srv_tgt = pkt[9] | (pkt[10] << 8);
        uint8_t  cid     = srv_tgt & 0xFF;
        uint8_t  flags   = pkt[2];
        const uint8_t *payload = pkt + 13;
        uint16_t payload_len   = len - 13;

        // Address filter (broadcast or our SN)
        uint32_t sn = *(volatile uint32_t *)(0x1FFFF7E8);
        uint16_t our_addr = (uint16_t)(sn & 0xFFFF);
        if (id_tgt != ADDR_BROADCAST && id_tgt != our_addr) continue;
        if ((srv_tgt >> 8) != 0x00) continue;  // must be Bootloader service

        uint8_t reply[8];
        uint8_t reply_len = 0;

        switch (cid)
        {
        case 0: // Bootloader Check
            if (payload_len >= 4) {
                memcpy(reply, payload, 4);
                reply[4] = 1;  // true = in bootloader
                reply_len = 5;
            }
            break;

        case 1: // App Write (FRAG stream)
            if (flags & 0x10) {  // FLAG_FRAG: payload has frag info
                uint16_t frag_cur = payload[0] | (payload[1] << 8);
                const uint8_t *data = payload + 4;
                uint16_t data_len = payload_len - 4;

                if (frag_cur == 0) {
                    flash_erase_app();
                    app_offset = 0;
                    first_frag = false;
                }

                if (flash_write(app_offset, data, data_len))
                    app_offset += data_len;

                reply[0] = frag_cur & 0xFF;
                reply[1] = (frag_cur >> 8) & 0xFF;
                reply_len = 2;
            }
            break;
        }

        if (reply_len > 0 && (flags & FLAG_REQACK))
            send_response(pkt, reply, reply_len);
    }
}

#endif // BOOTLOADER_BINARY
