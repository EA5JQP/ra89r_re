#include "driver/keypad.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"

/* ------------------------------------------------------------------------- */
/* the decode table                                                          */
/* ------------------------------------------------------------------------- */

/* One row per key, exactly as extracted from the stock handlers (each row is
 * one of them, 0x080146A0..0x08014B00): the window a line's level must fall in
 * -- exclusive low, inclusive high -- and the codes that window produces.  A
 * window with 0xFF for the second and third code is a key the stock app only
 * reports as a short press (the digits). */
typedef struct {
    uint8_t line;
    uint16_t lo, hi;
    uint8_t code[3];            /* short, long, extra-long; 0xFF = none */
} kp_window_t;

#define W(line, lo, hi, c0, c1, c2) { KEYPAD_LINE_##line, lo, hi, { c0, c1, c2 } }

static const kp_window_t windows[] = {
    /* PA2 = ADC rank 0 (channel 2): the programmable keys plus PTT1, which is
     * the key that pulls this line fully low and is also read digitally. */
    W(PA2, 0x0000, 0x007C, 0x64, 0xFF, 0xFF),
    W(PA2, 0x04AA, 0x05A2, 0x04, 0x05, 0x06),
    W(PA2, 0x074E, 0x0846, 0x07, 0x08, 0x09),

    /* PA3 = rank 1 (channel 3): digits 9 and 0, plus two function keys. */
    W(PA3, 0x0000, 0x007C, 0x13, 0xFF, 0xFF),
    W(PA3, 0x0384, 0x047C, 0x19, 0x1F, 0x25),
    W(PA3, 0x08B2, 0x09AA, 0x0A, 0xFF, 0xFF),
    W(PA3, 0x0ABB, 0x0BB3, 0x18, 0x1E, 0x24),

    /* PA6 = rank 2 (channel 6): four function keys. */
    W(PA6, 0x0000, 0x007C, 0x17, 0x1D, 0x23),
    W(PA6, 0x0384, 0x047C, 0x15, 0x1B, 0x21),
    W(PA6, 0x08B2, 0x09AA, 0x16, 0x1C, 0x22),
    W(PA6, 0x0ABB, 0x0BB3, 0x14, 0x1A, 0x20),

    /* PA7 = rank 3 (channel 7): digits 3, 2, 1 and 4. */
    W(PA7, 0x0000, 0x007C, 0x0D, 0xFF, 0xFF),
    W(PA7, 0x0384, 0x047C, 0x0C, 0xFF, 0xFF),
    W(PA7, 0x08B2, 0x09AA, 0x0B, 0xFF, 0xFF),
    W(PA7, 0x0ABB, 0x0BB3, 0x0E, 0xFF, 0xFF),

    /* PB0 = rank 4 (channel 8): digits 6, 5, 8 and 7. */
    W(PB0, 0x0000, 0x007C, 0x10, 0xFF, 0xFF),
    W(PB0, 0x0384, 0x047C, 0x0F, 0xFF, 0xFF),
    W(PB0, 0x08B2, 0x09AA, 0x12, 0xFF, 0xFF),
    W(PB0, 0x0ABB, 0x0BB3, 0x11, 0xFF, 0xFF),
};

#define NWINDOWS (sizeof(windows) / sizeof(windows[0]))

/* line -> ADC channel and the pin it is (for the monitor output) */
typedef struct {
    uint8_t channel;
    const char *name;
} kp_line_t;

static void keypad_init_ptt2(void);
static uint16_t adc_sample(unsigned channel);

static void adc_calibrate(void)
{
    uint32_t t0 = systick_millis();

    /* Reset and run the ADC calibration, exactly as the stock driver does
     * (0x080108C0 sets RSTCAL, waits, then CAL, waits).  The windows below are
     * that driver's calibration, so this side has to match it. */
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL) {
        if ((uint32_t)(systick_millis() - t0) > 20u)
            return;                         /* never hang */
    }
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL) {
        if ((uint32_t)(systick_millis() - t0) > 20u)
            return;
    }
}
static const kp_line_t lines[KEYPAD_LINE_COUNT] = {
    { 2, "PA2" }, { 3, "PA3" }, { 6, "PA6" }, { 7, "PA7" }, { 8, "PB0" },
};

static uint16_t s_raw[KEYPAD_LINE_COUNT];

/* ------------------------------------------------------------------------- */
/* ADC                                                                       */
/* ------------------------------------------------------------------------- */

/* 480 cycles -- the ladders are high impedance, so the sample capacitor needs
 * the longest settling time the part offers (SMP field 7). */
#define SAMPLE_TIME 7u

/* SWSTART as the regular-channel trigger source (EXTSEL = 0b111).  This is not
 * optional: with EXTSEL left at 0 (TIM1_CC1) a SWSTART write starts nothing, the
 * EOC flag never sets and every read times out.  The stock driver writes the same
 * value -- 0xE0000 into CR2 (it clears bits 17..20 first, mask 0xFFE1F7FD). */
#define EXTSEL_SWSTART (7u << ADC_CR2_EXTSEL_Pos)

bool keypad_init(void)
{
    unsigned i;

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;

    ADC1->CR1 = 0;                          /* 12-bit, no scan, no interrupts */
    ADC1->CR2 = EXTSEL_SWSTART;             /* software start as the trigger */
    for (i = 0; i < KEYPAD_LINE_COUNT; i++) {
        unsigned ch = lines[i].channel;
        unsigned sh = (ch % 10u) * 3u;
        if (ch < 10u)
            ADC1->SMPR2 = (ADC1->SMPR2 & ~(7u << sh)) | (SAMPLE_TIME << sh);
        else
            ADC1->SMPR1 = (ADC1->SMPR1 & ~(7u << sh)) | (SAMPLE_TIME << sh);
    }
    ADC1->SQR1 = 0;                         /* one conversion in the sequence */

    ADC1->CR2 |= ADC_CR2_ADON;              /* wake the ADC */
    (void)ADC1->CR2;
    systick_delay_ms(1);
    adc_calibrate();

    keypad_init_ptt2();

    /* Prove it converts: a conversion that never finishes is otherwise silent
     * (every read would just report 0xFFFF). */
    return adc_sample(lines[0].channel) != 0xFFFFu;
}

static void keypad_init_ptt2(void)
{
    /* PB9 is already a plain input (main.c parks the keypad pins); nothing to do
     * beyond making sure its port clock is on for the read. */
    gpio_port_clock(KEYPAD_PTT2_PORT);
}
static uint16_t adc_sample(unsigned channel)
{
    uint32_t t0 = systick_millis();

    ADC1->SQR3 = channel & 0x1Fu;
    ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {
        if ((uint32_t)(systick_millis() - t0) > 5u)
            return 0xFFFFu;                 /* never hang on a dead ADC */
    }
    return (uint16_t)(ADC1->DR & 0xFFFFu);
}

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

static const uint8_t *window_for(uint8_t line, uint16_t value)
{
    unsigned i;

    for (i = 0; i < NWINDOWS; i++) {
        if (windows[i].line != line)
            continue;
        if (value > windows[i].lo && value <= windows[i].hi)
            return windows[i].code;
    }
    return 0;
}

int keypad_scan(void)
{
    const uint8_t *code = 0;
    unsigned line;

    for (line = 0; line < KEYPAD_LINE_COUNT; line++) {
        uint16_t v = adc_sample(lines[line].channel);

        s_raw[line] = v;
        if (!code)
            code = window_for((uint8_t)line, v);
    }
    return code ? (int)code[0] : KEYPAD_NONE;
}

uint16_t keypad_raw(unsigned line)
{
    return line < KEYPAD_LINE_COUNT ? s_raw[line] : 0;
}

const char *keypad_line_name(unsigned line)
{
    return line < KEYPAD_LINE_COUNT ? lines[line].name : "?";
}

const uint8_t *keypad_variants(int code)
{
    unsigned i;

    for (i = 0; i < NWINDOWS; i++) {
        if (windows[i].code[0] == (uint8_t)code)
            return windows[i].code;
    }
    return 0;
}

const char *keypad_name(int code)
{
    static char buf[20];

    if (code == KEYPAD_NONE)
        return "nothing";
    if (code >= 10 && code <= 19) {
        buf[0] = 'd';
        buf[1] = 'i';
        buf[2] = 'g';
        buf[3] = 'i';
        buf[4] = 't';
        buf[5] = ' ';
        buf[6] = (char)('0' + (code - 10));
        buf[7] = '\0';
        return buf;
    }
    switch (code) {
    case 0x20: return "select/menu";
    case 0x21: return "up";
    case 0x22: return "down";
    case 100:  return "PTT1";
    default:   break;
    }
    if (code >= 4 && code <= 9)
        return "programmable key";
    /* the code itself is the useful part for the rest */
    buf[0] = 'c'; buf[1] = 'o'; buf[2] = 'd'; buf[3] = 'e'; buf[4] = ' ';
    buf[5] = '0'; buf[6] = 'x';
    buf[7] = "0123456789ABCDEF"[(code >> 4) & 0xF];
    buf[8] = "0123456789ABCDEF"[code & 0xF];
    buf[9] = '\0';
    return buf;
}

bool keypad_ptt2_level(void)
{
    return gpio_read(KEYPAD_PTT2_PORT, KEYPAD_PTT2_PIN) ? true : false;
}
