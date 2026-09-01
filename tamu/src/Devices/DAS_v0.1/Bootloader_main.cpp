// DAS v0.1 Standalone Bootloader — reuses HAL init from the normal DAS app.
//
// Same init sequence as DAS Main.h + RSBus.h SetupRS485().
// Same timing as DAS Base.h TimeFromBoot().
// Control-character UART protocol (Docs/Services/Bootloader.md).

#ifdef BOOTLOADER_BINARY

#include "ch32v00x.h"
#include "debug.h"
#include <cstdint>
#include <cstring>

// === Constants ===

#define APP_START     0x1000u
#define STORAGE_START 0x3800u
#define APP_MAX_SIZE  (STORAGE_START - APP_START)
#define FLASH_CTRL    0x08000000u

#define LEDR_PIN   GPIO_Pin_1  // PA1
#define LEDW_PIN   GPIO_Pin_0  // PD0
#define BTN_PIN    GPIO_Pin_0  // PC0
#define RS485_EN   GPIO_Pin_4  // PD4

#define CTRL_ENUM    'E'
#define CTRL_CONFIRM 'C'
#define CTRL_WRITE   'W'
#define CTRL_DONE    'D'
#define CTRL_VERIFY  'V'
#define CTRL_READ    'R'

#define ENUM_PAYLOAD_SIZE 16u
#define FRAG_SIZE      256u
#define FRAG_IDX_SIZE  4u
#define DEVICE_TYPE_DAS 0x03u

// === Flash register block (no HAL for flash write on CH32V003) ===

struct FLASH_r {
    volatile uint32_t ACTLR, KEYR, OBKEYR, STATR, CTLR, ADDR, RESERVED, OBR, WPR, MODEKEYR, BOOT_MODEKEYR;
};
#define FLASHr ((FLASH_r *)0x40022000)
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu
#define FLASH_BSY      (1u << 0)
#define FLASH_PG       (1u << 0)
#define FLASH_STRT     (1u << 6)
#define FLASH_LOCK_BIT (1u << 7)

// === Timing — copied from DAS Base.h TimeFromBoot() ===

static uint32_t ms_accum = 0;
static uint32_t last_cnt = 0;
static uint32_t ms_rem = 0;

uint32_t TimeFromBoot(void) {
    static bool s_inited = false;
    uint32_t current_cnt = SysTick->CNT;
    if (!s_inited) {
        last_cnt = current_cnt;
        s_inited = true;
    }
    uint32_t elapsed = current_cnt - last_cnt;
    last_cnt = current_cnt;
    uint32_t divisor = (SystemCoreClock / 1000);
    uint32_t total = elapsed + ms_rem;
    ms_rem = total % divisor;
    ms_accum += total / divisor;
    return ms_accum;
}

void Sleep(uint32_t ms) {
    uint32_t start = TimeFromBoot();
    while ((TimeFromBoot() - start) < ms) ;
}

// === LED — same macros as DAS Base.h ===

void PinModeOutput(GPIO_TypeDef* port, uint16_t pin) {
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &GPIO_InitStructure);
}

void PinHigh(GPIO_TypeDef* port, uint16_t pin) {
    GPIO_WriteBit(port, pin, Bit_SET);
}

void PinLow(GPIO_TypeDef* port, uint16_t pin) {
    GPIO_WriteBit(port, pin, Bit_RESET);
}

// === UART — copied from DAS RSBus.h SetupRS485() without IRQ ===

static void uart_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1, ENABLE);

    // USART1 TX-->D.5   RX-->D.6
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = RS485_EN;
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

    USART_Cmd(USART1, ENABLE);
}

static void uart_tx_enable(void)  { GPIO_WriteBit(GPIOD, RS485_EN, Bit_SET); }
static void uart_tx_disable(void) {
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
    GPIO_WriteBit(GPIOD, RS485_EN, Bit_RESET);
}

static void uart_send_byte(uint8_t b) {
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, b);
}

static void uart_send(const uint8_t *data, uint16_t len) {
    uart_tx_enable();
    for (uint16_t i = 0; i < len; i++) uart_send_byte(data[i]);
    uart_tx_disable();
}

static int uart_recv_byte(uint32_t timeout_ms) {
    uint32_t start = TimeFromBoot();
    while (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == RESET) {
        if ((TimeFromBoot() - start) >= timeout_ms) return -1;
    }
    return (int)(uint8_t)USART_ReceiveData(USART1);
}

static bool uart_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms) {
    for (uint16_t i = 0; i < len; i++) {
        int b = uart_recv_byte(timeout_ms);
        if (b < 0) return false;
        buf[i] = (uint8_t)b;
    }
    return true;
}

// === Flash ===

static void flash_unlock(void) {
    FLASHr->KEYR = FLASH_KEY1;
    FLASHr->KEYR = FLASH_KEY2;
    FLASHr->MODEKEYR = FLASH_KEY1;
    FLASHr->MODEKEYR = FLASH_KEY2;
}
static void flash_lock(void) { FLASHr->CTLR |= FLASH_LOCK_BIT; }

static void flash_erase_app(void) {
    flash_unlock();
    for (uint32_t addr = FLASH_CTRL + APP_START; addr < FLASH_CTRL + STORAGE_START; addr += 64) {
        FLASHr->ADDR = addr;
        FLASHr->CTLR |= (1u << 17) | FLASH_STRT;
        while (FLASHr->STATR & FLASH_BSY) ;
    }
    flash_lock();
}

static bool flash_write(uint32_t offset, const uint8_t *data, uint32_t len) {
    if (offset + len > APP_MAX_SIZE || len == 0) return false;
    uint32_t faddr = FLASH_CTRL + APP_START + offset;
    flash_unlock();
    uint32_t start = faddr & ~3u;
    uint32_t end = (faddr + len + 3u) & ~3u;
    for (uint32_t a = start; a < end; a += 4) {
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

static void flash_read(uint32_t offset, uint8_t *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)(FLASH_CTRL + APP_START + offset);
    for (uint32_t i = 0; i < len; i++) buf[i] = src[i];
}

// === Jump to application ===

static void jump_to_app(void) {
    PinLow(GPIOD, LEDW_PIN);
    PinLow(GPIOA, LEDR_PIN);

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

// === Main ===

int main(void)
{
    // Same init as DAS Main.h
    SystemCoreClockUpdate();
    Delay_Init();
    SysTick->CTLR |= 0x05;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

    PinModeOutput(GPIOA, LEDR_PIN);
    PinModeOutput(GPIOD, LEDW_PIN);
    PinLow(GPIOD, LEDW_PIN);
    PinLow(GPIOA, LEDR_PIN);

    // Button: PC0 input pull-up
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = BTN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    if (GPIO_ReadInputDataBit(GPIOC, BTN_PIN))
        jump_to_app();

    // === Bootloader mode ===
    uart_init();
    PinHigh(GPIOD, LEDW_PIN);

    // Build enumeration payload
    uint8_t enum_payload[ENUM_PAYLOAD_SIZE];
    {
        uint16_t dev_type = DEVICE_TYPE_DAS;
        enum_payload[0] = dev_type & 0xFF;
        enum_payload[1] = (dev_type >> 8) & 0xFF;
        const uint8_t *chip_id = (const uint8_t *)0x1FFFF7E8;
        memset(enum_payload + 2, 0, 14);
        for (int i = 0; i < 8 && i < 14; i++)
            enum_payload[2 + i] = chip_id[i];
    }

    // Periodic enumeration + LED blink, using TimeFromBoot() ms timestamps
    bool confirmed = false;
    uint32_t last_blink_ms = TimeFromBoot();
    uint32_t last_enum_ms = TimeFromBoot();
    bool led_on = true;

    while (!confirmed)
    {
        uint32_t now_ms = TimeFromBoot();

        // Blink LED every 250ms
        if (now_ms - last_blink_ms >= 250) {
            led_on = !led_on;
            if (led_on) PinHigh(GPIOD, LEDW_PIN);
            else        PinLow(GPIOD, LEDW_PIN);
            last_blink_ms = now_ms;
        }

        // Send enumeration every 1s
        if (now_ms - last_enum_ms >= 1000) {
            uart_tx_enable();
            uart_send_byte(CTRL_ENUM);
            for (int i = 0; i < ENUM_PAYLOAD_SIZE; i++)
                uart_send_byte(enum_payload[i]);
            uart_tx_disable();
            last_enum_ms = now_ms;
        }

        // Quick poll for confirm
        int b = uart_recv_byte(5);
        if (b == CTRL_CONFIRM)
            confirmed = true;
    }

    // Main loop: receive commands, respond
    uint8_t idx_buf[FRAG_IDX_SIZE];
    uint8_t data_buf[FRAG_SIZE];

    while (1)
    {
        uint32_t now_ms = TimeFromBoot();

        // Blink LED
        if (now_ms - last_blink_ms >= 250) {
            led_on = !led_on;
            if (led_on) PinHigh(GPIOD, LEDW_PIN);
            else        PinLow(GPIOD, LEDW_PIN);
            last_blink_ms = now_ms;
        }

        int b = uart_recv_byte(10);
        if (b < 0) continue;

        uint8_t ctrl = (uint8_t)b;

        switch (ctrl)
        {
        case CTRL_WRITE:
        {
            if (!uart_recv(idx_buf, FRAG_IDX_SIZE, 100))
                break;
            if (!uart_recv(data_buf, FRAG_SIZE, 500))
                break;

            uint32_t frag_idx = idx_buf[0] | ((uint32_t)idx_buf[1] << 8) |
                                ((uint32_t)idx_buf[2] << 16) | ((uint32_t)idx_buf[3] << 24);

            if (frag_idx == 0) {
                PinHigh(GPIOA, LEDR_PIN);
                flash_erase_app();
            }

            uint32_t offset = frag_idx * FRAG_SIZE;
            uint32_t write_len = FRAG_SIZE;
            if (offset + write_len > APP_MAX_SIZE)
                write_len = APP_MAX_SIZE - offset;

            PinHigh(GPIOA, LEDR_PIN);
            flash_write(offset, data_buf, write_len);
            PinLow(GPIOA, LEDR_PIN);

            uint8_t resp[5];
            resp[0] = CTRL_DONE;
            resp[1] = idx_buf[0]; resp[2] = idx_buf[1];
            resp[3] = idx_buf[2]; resp[4] = idx_buf[3];
            uart_send(resp, sizeof(resp));
            break;
        }

        case CTRL_VERIFY:
        {
            if (!uart_recv(idx_buf, FRAG_IDX_SIZE, 100))
                break;

            uint32_t frag_idx = idx_buf[0] | ((uint32_t)idx_buf[1] << 8) |
                                ((uint32_t)idx_buf[2] << 16) | ((uint32_t)idx_buf[3] << 24);

            uint32_t offset = frag_idx * FRAG_SIZE;
            uint32_t read_len = FRAG_SIZE;
            if (offset + read_len > APP_MAX_SIZE)
                read_len = APP_MAX_SIZE - offset;

            uint8_t read_buf[FRAG_SIZE];
            memset(read_buf, 0xFF, FRAG_SIZE);
            flash_read(offset, read_buf, read_len);

            uint8_t hdr[5];
            hdr[0] = CTRL_READ;
            hdr[1] = idx_buf[0]; hdr[2] = idx_buf[1];
            hdr[3] = idx_buf[2]; hdr[4] = idx_buf[3];

            uart_tx_enable();
            for (int i = 0; i < 5; i++) uart_send_byte(hdr[i]);
            for (uint32_t i = 0; i < FRAG_SIZE; i++) uart_send_byte(read_buf[i]);
            uart_tx_disable();
            break;
        }

        default:
            break;
        }
    }
}

#endif // BOOTLOADER_BINARY
