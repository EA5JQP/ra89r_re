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
 * `key` is the K5V3 name this button takes in the port.  The mapping is the
 * owner's (F -> KEY_MENU, AB -> KEY_EXIT, # -> KEY_F, the side pair -> the
 * KEY_SIDEx pair) bound to codes as follows:
 *
 *  - digits, PTT and PTT2 were read straight off the radio with the monitor;
 *  - UP / DOWN: the stock list widget uses their held codes 0x21 / 0x22 as
 *    list-up / list-down;
 *  - F (0x14) and AB (0x17): settled twice over.  The stock handlers put the
 *    menu on 0x14 (held code 0x20 -> FUN_0800C41C) and the invalid/back beep on
 *    0x17 (its held/extra codes), and the owner's sweep of every key agrees:
 *    F is 0x14 (its window is the D tap) and AB is 0x17 (the A tap).  This pair
 *    was briefly bound the other way round on the strength of a spoken label in a
 *    single-key test -- not evidence.  A sweep of *every* key settles a mapping;
 *    that should have been the first test, not the last;
 *  - SIDE1 / SIDE2 (4-6 / 7-9): these are the only codes with three press
 *    types, which is what the CPS's "Side1 / Side2 Short/Long" settings name;
 *  - * (0x18) and # (0x19): the remaining pair, placed by where they sit in the
 *    keypad row (9 * 0 #) as read off the radio.
 *
 * The last four are placements by elimination, so they are the ones to check
 * first: the monitor prints the code next to the name, so pressing the key is
 * enough to see whether this table agrees with the silkscreen. */
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
    W(PA2, 0x04AA, 0x05A2, 0x04, 0x05, 0x06, KEY_SIDE1),
    W(PA2, 0x074E, 0x0846, 0x07, 0x08, 0x09, KEY_SIDE2),

    /* PA3 = rank 1 (channel 3): digits 9 and 0, plus two function keys. */
    W(PA3, 0x0000, 0x007C, 0x13, 0xFF, 0xFF, KEY_9),
    W(PA3, 0x0384, 0x047C, 0x19, 0x1F, 0x25, KEY_F),
    W(PA3, 0x08B2, 0x09AA, 0x0A, 0xFF, 0xFF, KEY_0),
    W(PA3, 0x0ABB, 0x0BB3, 0x18, 0x1E, 0x24, KEY_STAR),

    /* PA6 = rank 2 (channel 6): four function keys. */
    W(PA6, 0x0000, 0x007C, 0x17, 0x1D, 0x23, KEY_EXIT),
    W(PA6, 0x0384, 0x047C, 0x15, 0x1B, 0x21, KEY_UP),    /* held 0x21 = up */
    W(PA6, 0x08B2, 0x09AA, 0x16, 0x1C, 0x22, KEY_DOWN),  /* held 0x22 = down */
    W(PA6, 0x0ABB, 0x0BB3, 0x14, 0x1A, 0x20, KEY_MENU),

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

/* ------------------------------------------------------------------------- */
/* ADC -- free-running scan through DMA, as the stock application does        */
/* ------------------------------------------------------------------------- */

/* The stock does not convert on demand: it leaves the ADC scanning six channels
 * continuously into memory through DMA (`FUN_08010DB8` starts it, and the
 * accessor it later calls, `FUN_08024260`, averages the eight samples of one
 * channel out of that buffer).  That is worth copying rather than reimplementing:
 * one conversion with the longest sample time takes ~123 us, so a five-line scan
 * that waited for its own conversions cost ~5 ms -- half of the 10 ms tick the
 * K5V3 application polls the keypad on.  Free-running, a read is a memory access.
 *
 * The buffer holds **one round** of the six channels, and the reader takes the
 * newest value of each line rather than the eight-round average the stock uses.
 * That is deliberate, and it was measured rather than reasoned: with the stock's
 * longer window the press and release *edges* mis-read, because the window still
 * holds pre-press samples and their fraction of a tap level lands inside a
 * neighbouring window -- pressing one key reported its neighbours as keys.  The
 * stock can afford the long window because its own 4-consecutive rule debounces
 * on top; here the debouncing is the application layer's job (the K5V3's
 * APP_TimeSlice10ms counts stable polls), which wants the opposite input: the
 * level as it is now, at most one round (~740 us) old.
 *
 * 32-bit slots, as the stock's DMA (PSIZE/MSIZE = 32 bit) -- a channel is 4 bytes
 * from the next.  Channel 6 (PB1) is scanned and unused by the keypad: it is what
 * the stock's S-meter reads, and scanning it keeps the sequence the vendor's.
 */
#define KP_CHANNELS 6u                          /* channels per round = DMA count */

/* 480 cycles -- the ladders are high impedance, so the sample capacitor needs
 * the longest settling time the part offers (SMP field 7).  Free-running, that
 * costs nothing: one round of six conversions takes ~740 us in the background. */
#define SAMPLE_TIME 7u

/* SWSTART as the regular-channel trigger source (EXTSEL = 0b111).  This is not
 * optional: with EXTSEL left at 0 (TIM1_CC1) a SWSTART write starts nothing, the
 * EOC flag never sets and every read times out.  The stock driver writes the same
 * value -- 0xE0000 into CR2 (it clears bits 17..20 first, mask 0xFFE1F7FD), and
 * it too only sets EXTTRIG when EXTSEL selects SWSTART. */
#define EXTSEL_SWSTART (7u << ADC_CR2_EXTSEL_Pos)

static const kp_line_t lines[KEYPAD_LINE_COUNT] = {
    { 2, "PA2" }, { 3, "PA3" }, { 6, "PA6" }, { 7, "PA7" }, { 8, "PB0" },
};

/* the six scanned channels, in sequence order (the first five are the lines) */
static const uint8_t channels[KP_CHANNELS] = { 2, 3, 6, 7, 8, 9 };

/* DMA target: one 32-bit slot per channel, overwritten every round */
static uint32_t s_dma[KP_CHANNELS];

static uint16_t s_raw[KEYPAD_LINE_COUNT];

static void adc_calibrate(void)
{
    uint32_t t0 = systick_millis();

    /* Reset and run the ADC calibration, exactly as the stock driver does
     * (0x080108C0 sets RSTCAL, waits, then CAL, waits).  The windows are that
     * driver's calibration, so this side has to match it.  Done before the scan
     * starts, since a running sequence must not be disturbed. */
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

bool keypad_init(void)
{
    uint32_t t0, before;
    unsigned i;
    uint32_t sqr = 0;
    bool any = false;

    /* ADCCLK = PCLK2/2.  Worth setting rather than inheriting: we never touch
     * RCC_CFGR's ADCPRE elsewhere, so its value is whatever the bootloader left,
     * and the sample time (hence how the levels compare with the stock windows)
     * depends on it. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV2;

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN | RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;

    /* Choose the request: this part has no DMA request-select register, it maps
     * requests in SYSCFG instead -- DMA1 channel 1 is the 7-bit field at the
     * bottom of SYSCFG->CFGR[2], and the value for ADC1 is 0
     * (LL_SYSCFG_DMA_MAP_ADC1), i.e. the reset default restated.  Both the SDK's
     * ADC+DMA example (Projects/.../ADC_MultiChannelSingleConversion_TriggerSW_DMA)
     * and the vendor's own DMA driver write it, so do it rather than trust the
     * default.  Field preserved for the other channels. */
    SYSCFG->CFGR[2] = SYSCFG->CFGR[2] & ~0x7Fu;

    ADC1->CR1 = ADC_CR1_SCAN;               /* scan the sequence, 12-bit */
    ADC1->CR2 = EXTSEL_SWSTART;             /* software start as the trigger */
    for (i = 0; i < KP_CHANNELS; i++) {
        unsigned ch = channels[i];
        unsigned sh = (ch % 10u) * 3u;      /* all six are < 10 -> SMPR2 only */
        ADC1->SMPR2 = (ADC1->SMPR2 & ~(7u << sh)) | (SAMPLE_TIME << sh);
        sqr |= (uint32_t)ch << (5u * i);    /* SQR3: SQ1..SQ6, 5 bits each */
    }
    ADC1->SQR1 = (KP_CHANNELS - 1u) << ADC_SQR1_L_Pos;   /* six conversions */
    ADC1->SQR3 = sqr;

    ADC1->CR2 |= ADC_CR2_ADON;              /* wake the ADC */
    (void)ADC1->CR2;
    systick_delay_ms(1);
    adc_calibrate();

    /* ADC1's DMA request is wired to DMA1 channel 1 on this part (there is no
     * request-select register).  32-bit both sides, memory increment, circular,
     * very high priority -- the stock's DMA configuration. */
    DMA1_Channel1->CCR = 0;
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)s_dma;
    DMA1_Channel1->CNDTR = KP_CHANNELS;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    DMA1_Channel1->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PSIZE_1 |
                         DMA_CCR_MSIZE_1 | DMA_CCR_PL;
    DMA1_Channel1->CCR |= DMA_CCR_EN;

    ADC1->CR2 |= ADC_CR2_DMA | ADC_CR2_CONT;    /* stream every conversion */
    ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART; /* kick the sequence off */

    keypad_init_ptt2();

    /* Prove the scan is really running, and that it is *writing*: a dead DMA
     * leaves the buffer at zero, and an all-zero read is indistinguishable from
     * a held PTT1 (its window starts at 0), which would be a phantom keypress.
     * Both checks are cheap and neither can false-fail: the idle levels are near
     * full scale, so a round of six real conversions is never all zero. */
    t0 = systick_millis();
    before = DMA1_Channel1->CNDTR;
    while (DMA1_Channel1->CNDTR == before) {
        if ((uint32_t)(systick_millis() - t0) > 5u)
            return false;                   /* the transfers are not moving */
    }
    for (i = 0; i < KP_CHANNELS; i++)
        any = any || (s_dma[i] & 0xFFFu) != 0u;
    return any;
}

static void keypad_init_ptt2(void)
{
    /* PB9 is already a plain input (main.c parks the keypad pins); nothing to do
     * beyond making sure its port clock is on for the read. */
    gpio_port_clock(KEYPAD_PTT2_PORT);
}
/* Newest conversion of one line.  Non-blocking: it is a memory read, and at most
 * one round old -- the DMA writes all six slots and wraps. */
static uint16_t line_value(unsigned line)
{
    return (uint16_t)(s_dma[line] & 0xFFFu);
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
        uint16_t v = line_value(line);

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

/* The sixth scanned channel (PB1 / ADC channel 9) -- the battery sense.  The
 * keypad does not use it; the battery driver does. */
uint16_t keypad_aux_raw(void)
{
    return (uint16_t)(s_dma[KP_CHANNELS - 1u] & 0xFFFu);
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
