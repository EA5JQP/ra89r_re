#include "driver/keypad.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"

/* ------------------------------------------------------------------------- */
/* the decode table                                                          */
/* ------------------------------------------------------------------------- */

/* One row per key, exactly as extracted from the stock handlers (each row is
 * one of them, 0x080146A0..0x08014B00): the window a line's level must fall in
 * -- exclusive low, inclusive high -- the codes that window produces, and the
 * K5V3 key it is.  code[0] is what the stock app posts at the 4th in-window
 * sample (a short press), code[1] when held to the 40th, code[2] on release;
 * 0xFF = the window has no such code (the digits, for instance).
 *
 * `key` is KEY_INVALID for the codes whose stock action is not conclusive yet:
 * those buttons are unlabelled in the firmware, so the mapping has to be read
 * off the radio (press it, note the code).  UP and DOWN are settled because the
 * stock dispatcher uses their *held* codes (0x21 / 0x22) as list-up / list-down. */
typedef struct {
    uint8_t line;
    uint16_t lo, hi;
    uint8_t code[3];            /* short, held, long; 0xFF = none */
    KEY_Code_t key;
} kp_window_t;

#define W(line, lo, hi, c0, c1, c2, k) \
    { KEYPAD_LINE_##line, lo, hi, { c0, c1, c2 }, k }

static const kp_window_t windows[] = {
    /* PA2 = ADC rank 0 (channel 2): the programmable keys plus PTT1, which is
     * the key that pulls this line fully low and is also read digitally. */
    W(PA2, 0x0000, 0x007C, 0x64, 0xFF, 0xFF, KEY_PTT),
    W(PA2, 0x04AA, 0x05A2, 0x04, 0x05, 0x06, KEY_INVALID),/* TODO: P1/P2/SIDE ? */
    W(PA2, 0x074E, 0x0846, 0x07, 0x08, 0x09, KEY_INVALID),/* TODO */

    /* PA3 = rank 1 (channel 3): digits 9 and 0, plus two function keys. */
    W(PA3, 0x0000, 0x007C, 0x13, 0xFF, 0xFF, KEY_9),
    W(PA3, 0x0384, 0x047C, 0x19, 0x1F, 0x25, KEY_INVALID),/* TODO */
    W(PA3, 0x08B2, 0x09AA, 0x0A, 0xFF, 0xFF, KEY_0),
    W(PA3, 0x0ABB, 0x0BB3, 0x18, 0x1E, 0x24, KEY_INVALID),/* TODO (0x18 -> volume) */

    /* PA6 = rank 2 (channel 6): four function keys. */
    W(PA6, 0x0000, 0x007C, 0x17, 0x1D, 0x23, KEY_INVALID),/* TODO */
    W(PA6, 0x0384, 0x047C, 0x15, 0x1B, 0x21, KEY_UP),    /* held 0x21 = up */
    W(PA6, 0x08B2, 0x09AA, 0x16, 0x1C, 0x22, KEY_DOWN),  /* held 0x22 = down */
    W(PA6, 0x0ABB, 0x0BB3, 0x14, 0x1A, 0x20, KEY_INVALID),/* TODO (long 0x20 = select/menu) */

    /* PA7 = rank 3 (channel 7): digits 3, 2, 1 and 4. */
    W(PA7, 0x0000, 0x007C, 0x0D, 0xFF, 0xFF, KEY_3),
    W(PA7, 0x0384, 0x047C, 0x0C, 0xFF, 0xFF, KEY_2),
    W(PA7, 0x08B2, 0x09AA, 0x0B, 0xFF, 0xFF, KEY_1),
    W(PA7, 0x0ABB, 0x0BB3, 0x0E, 0xFF, 0xFF, KEY_4),

    /* PB0 = rank 4 (channel 8): digits 6, 5, 8 and 7. */
    W(PB0, 0x0000, 0x007C, 0x10, 0xFF, 0xFF, KEY_6),
    W(PB0, 0x0384, 0x047C, 0x0F, 0xFF, 0xFF, KEY_5),
    W(PB0, 0x08B2, 0x09AA, 0x12, 0xFF, 0xFF, KEY_8),
    W(PB0, 0x0ABB, 0x0BB3, 0x11, 0xFF, 0xFF, KEY_7),
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

/* The stock application averages 8 samples per channel (0x08024260) and its
 * windows were measured with that filtering, so a single un-averaged conversion
 * is both noisier and not quite the same measurement. */
#define SAMPLES 8u

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
static uint16_t adc_sample_once(unsigned channel)
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

/* Average of SAMPLES conversions, like the stock accessor. */
static uint16_t adc_sample(unsigned channel)
{
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i < SAMPLES; i++) {
        uint16_t v = adc_sample_once(channel);

        if (v == 0xFFFFu)
            return 0xFFFFu;                 /* never hang on a dead ADC */
        sum += v;
    }
    return (uint16_t)(sum / SAMPLES);
}

/* ------------------------------------------------------------------------- */
/* decode                                                                    */
/* ------------------------------------------------------------------------- */

/* The vendor's windows are the calibration; WINDOW_MARGIN widens each one so a
 * level that lands just outside still decodes.  The bands are far apart (the
 * narrowest gap is 273 counts, between the third and fourth band) and the idle
 * level is near full scale, so 96 counts is safe. */
#define WINDOW_MARGIN 0x60

static const kp_window_t *window_for(uint8_t line, uint16_t value)
{
    unsigned i;

    for (i = 0; i < NWINDOWS; i++) {
        if (windows[i].line != line)
            continue;
        if ((int32_t)value > (int32_t)windows[i].lo - WINDOW_MARGIN &&
            (int32_t)value <= (int32_t)windows[i].hi + WINDOW_MARGIN)
            return &windows[i];
    }
    return 0;
}

static int s_stock = KEYPAD_NONE;

KEY_Code_t keypad_poll(void)
{
    const kp_window_t *win = 0;
    unsigned line;

    for (line = 0; line < KEYPAD_LINE_COUNT; line++) {
        uint16_t v = adc_sample(lines[line].channel);

        s_raw[line] = v;
        if (!win)
            win = window_for((uint8_t)line, v);
    }

    if (win) {
        s_stock = (int)win->code[0];
        return win->key;
    }

    s_stock = KEYPAD_NONE;
    /* PB9 is low while PTT2 is held; it moves no analog line, so it can only be
     * reported when no ladder key is down. */
    return keypad_ptt2_level() ? KEY_INVALID : KEY_PTT2;
}

int keypad_stock_code(void)
{
    return s_stock;
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

const char *keypad_name(KEY_Code_t key)
{
    switch (key) {
    case KEY_0: return "0";
    case KEY_1: return "1";
    case KEY_2: return "2";
    case KEY_3: return "3";
    case KEY_4: return "4";
    case KEY_5: return "5";
    case KEY_6: return "6";
    case KEY_7: return "7";
    case KEY_8: return "8";
    case KEY_9: return "9";
    case KEY_MENU: return "MENU";
    case KEY_UP: return "UP";
    case KEY_DOWN: return "DOWN";
    case KEY_EXIT: return "EXIT";
    case KEY_STAR: return "*";
    case KEY_F: return "F";
    case KEY_PTT: return "PTT";
    case KEY_SIDE2: return "SIDE2";
    case KEY_SIDE1: return "SIDE1";
    case KEY_PTT2: return "PTT2";
    default: return "?";            /* a code that is not mapped yet */
    }
}

const char *keypad_stock_name(int code)
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
