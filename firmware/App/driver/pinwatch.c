#include "driver/pinwatch.h"

#include "py32f4xx.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/uart.h"

/* Every pin the application does not need, so that the keypad (wherever it is)
 * shows up as a level change.  Excluded on purpose:
 *   PA8..PA11, PB15  the panel's bit-banged bus (keeps the screen readable)
 *   PB6, PB7         the console (keeps this output readable)
 * Nothing here is bonded to the MCU on every board revision, so a pin that is
 * simply floating reads 1 (pull-up) and can be ignored. */
typedef struct {
    uint8_t idx;                    /* 0 = A, 1 = B, 2 = C, 3 = D, 4 = E */
    uint8_t bit;
    GPIO_TypeDef *gpio;
    uint16_t mask;
} pin_t;

#define PIN(i, b, g) { (i), (b), (g), (uint16_t)(1u << (b)) }
#define NPINS (sizeof(pins) / sizeof(pins[0]))

static const pin_t pins[] = {
    PIN(0,  0, GPIOA), PIN(0,  1, GPIOA), PIN(0,  2, GPIOA), PIN(0,  3, GPIOA),
    PIN(0,  4, GPIOA), PIN(0,  5, GPIOA), PIN(0,  6, GPIOA), PIN(0,  7, GPIOA),
    PIN(0, 12, GPIOA), PIN(0, 13, GPIOA), PIN(0, 14, GPIOA), PIN(0, 15, GPIOA),
    PIN(1,  0, GPIOB), PIN(1,  1, GPIOB), PIN(1,  2, GPIOB), PIN(1,  3, GPIOB),
    PIN(1,  4, GPIOB), PIN(1,  5, GPIOB), PIN(1,  8, GPIOB), PIN(1,  9, GPIOB),
    PIN(1, 10, GPIOB), PIN(1, 11, GPIOB), PIN(1, 12, GPIOB), PIN(1, 13, GPIOB),
    PIN(1, 14, GPIOB),
    PIN(2, 13, GPIOC), PIN(2, 14, GPIOC), PIN(2, 15, GPIOC),
    PIN(3,  0, GPIOD), PIN(3,  1, GPIOD), PIN(3,  2, GPIOD),
    PIN(4,  0, GPIOE), PIN(4,  1, GPIOE), PIN(4,  2, GPIOE), PIN(4,  3, GPIOE),
    PIN(4,  4, GPIOE), PIN(4,  5, GPIOE), PIN(4,  6, GPIOE), PIN(4,  7, GPIOE),
    PIN(4,  8, GPIOE), PIN(4,  9, GPIOE), PIN(4, 10, GPIOE), PIN(4, 11, GPIOE),
    PIN(4, 12, GPIOE), PIN(4, 13, GPIOE), PIN(4, 14, GPIOE), PIN(4, 15, GPIOE),
};

static bool s_active;
static uint16_t s_last[5];

static void pin_letter(uint8_t idx, uint8_t bit)
{
    uart_printf("P%c%u", "ABCDE"[idx], (unsigned)bit);
}

static void snapshot(uint16_t *out)
{
    out[0] = (uint16_t)GPIOA->IDR;
    out[1] = (uint16_t)GPIOB->IDR;
    out[2] = (uint16_t)GPIOC->IDR;
    out[3] = (uint16_t)GPIOD->IDR;
    out[4] = (uint16_t)GPIOE->IDR;
}

static void set_input_pullup(const pin_t *p)
{
    uint32_t sh = (uint32_t)p->bit * 2u;

    gpio_port_clock(p->gpio);
    p->gpio->PUPDR = (p->gpio->PUPDR & ~(3u << sh)) | (1u << sh);
    p->gpio->OTYPER &= ~p->mask;
    p->gpio->AFR[p->bit >> 3] &= ~(0xFu << ((p->bit & 7u) * 4u));
    p->gpio->MODER &= ~(3u << sh);
}

void pinwatch_arm(void)
{
    unsigned i;
    uint16_t now[5];

    for (i = 0; i < NPINS; i++)
        set_input_pullup(&pins[i]);

    s_active = true;
    snapshot(now);
    for (i = 0; i < 5; i++)
        s_last[i] = now[i];

    uart_printf("\n[k] watching %u pins as input + pull-up (panel and console "
                "pins untouched)\n", (unsigned)NPINS);
    uart_printf("[k] baseline A=%04X B=%04X C=%04X D=%04X E=%04X\n",
                now[0], now[1], now[2], now[3], now[4]);
    uart_puts("[k] press ONE button at a time; every line that moves is printed\n"
              "    (a line going to 0 is the usual 'pressed')\n");
}

void pinwatch_stop(void)
{
    if (!s_active)
        return;
    s_active = false;
    uart_puts("[k] watcher off (a reset is needed to give the buses back to the "
              "radio)\n");
}

bool pinwatch_is_active(void)
{
    return s_active;
}

void pinwatch_poll(void)
{
    uint16_t now[5];
    unsigned i;
    bool changed = false;

    if (!s_active)
        return;

    snapshot(now);
    for (i = 0; i < 5; i++) {
        if (now[i] != s_last[i])
            changed = true;
    }
    if (!changed)
        return;

    uart_printf("[k %ums] A=%04X B=%04X C=%04X D=%04X E=%04X  moved:",
                (unsigned)systick_millis(), now[0], now[1], now[2], now[3], now[4]);
    for (i = 0; i < NPINS; i++) {
        uint16_t bit = pins[i].mask;
        uint16_t before = s_last[pins[i].idx] & bit;
        uint16_t after = now[pins[i].idx] & bit;

        if (before == after)
            continue;
        uart_puts(" ");
        pin_letter(pins[i].idx, pins[i].bit);
        uart_printf("=%u", after ? 1u : 0u);
    }
    uart_puts("\n");

    for (i = 0; i < 5; i++)
        s_last[i] = now[i];
}

void pinwatch_sweep(int passes)
{
    int pass;
    unsigned i, j;

    if (!s_active)
        pinwatch_arm();

    uart_printf("[k] open-drain sweep x%d -- hold a button down while it runs\n",
                passes);
    for (pass = 0; pass < passes; pass++) {
        for (i = 0; i < NPINS; i++) {
            const pin_t *row = &pins[i];
            uint32_t sh = (uint32_t)row->bit * 2u;
            uint16_t idr[5];

            /* open drain, so pulling low can never fight a line someone else
             * is driving high */
            row->gpio->OTYPER |= row->mask;
            row->gpio->MODER = (row->gpio->MODER & ~(3u << sh)) | (1u << sh);
            row->gpio->BRR = row->mask;
            systick_delay_ms(2);
            snapshot(idr);
            row->gpio->MODER &= ~(3u << sh);
            row->gpio->OTYPER &= ~row->mask;
            systick_delay_ms(1);

            for (j = 0; j < NPINS; j++) {
                if (j == i)
                    continue;
                if (!(idr[pins[j].idx] & pins[j].mask)) {
                    uart_printf("[k] while ");
                    pin_letter(row->idx, row->bit);
                    uart_puts(" is held low, ");
                    pin_letter(pins[j].idx, pins[j].bit);
                    uart_puts(" also reads 0 -- those two are connected\n");
                }
            }
        }
        uart_printf("[k] sweep %d/%d done\n", pass + 1, passes);
        if (uart_getc() >= 0)
            break;                      /* any console key stops the sweep */
    }
}
