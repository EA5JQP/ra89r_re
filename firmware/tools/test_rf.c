/* Host test for the two RF register layers, without a radio.
 *
 * This replaces the bus with a recording stub, so it checks the thing a
 * bring-up cannot check by looking at the screen: the exact bytes each part
 * will see, and whether the identity handshake is wired the way the stock's own
 * detect is (register 0 -> 0x4829 / 0x4816).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_rf.c App/driver/bk4829.c \
 *       App/driver/bk4815.c -o /tmp/test_rf && /tmp/test_rf
 *
 * It is not a substitute for the radio: it cannot tell whether the part answers
 * at all, only that we ask correctly.  `R` on the console is the other half.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/bk4815.h"
#include "driver/gpio.h"
#include "driver/pa.h"
#include "driver/rf_bus.h"
#include "driver/tx.h"
#include "settings.h"

/* Added with the stock-compatible BT GPIO implementation. */
extern void pa_set_bt_audio(bool on);
extern bool pa_bt_audio(void);

/* tx.c's BT hooks: this test is about register traffic, so the BT service is
 * stubbed rather than linked (which would drag in the EEPROM and the AT sink). */
void bt_set_radio_tx_active(bool active) { (void)active; }
bool bt_ptt_down(void) { return false; }
bool bt_ptt_source_active(bool radio_ptt, bool headset_ptt, uint8_t mode)
{
    (void)headset_ptt;
    (void)mode;
    return radio_ptt;
}
uint8_t bt_ptt_mode(bool bt_on, uint8_t type) { (void)bt_on; (void)type; return 1u; }

/* The GPIO/timer scratch the host device-header double points at.  This test
 * does not link tools/host/host_hw.c, so it supplies its own.  `pa_init()` is
 * never called, so `RCC`'s null pointer is never dereferenced. */
GPIO_TypeDef host_gpio_scratch[6];
TIM_TypeDef  host_tim_scratch;
RCC_TypeDef  host_rcc_scratch;   /* pa_init() references RCC; not called here */

/* `pa.c` calls these two in `pa_init()` only; the register test never brings
 * the timer up, so they are no-ops.  Stubbing them keeps the K1 hardware
 * surface out of this link (see `driver/gpio.c`). */
void gpio_port_clock(GPIO_TypeDef *port) { (void)port; }
void gpio_config_output(GPIO_TypeDef *port, uint32_t mask)
{
    (void)port;
    (void)mask;
}

/* `driver/tx.c` is linked so `tx_start()`'s register order can be asserted.
 * Only `tx_start()`/`tx_stop()` are called, so the application hooks the PTT
 * poller (`tx_poll_ptt()`) references -- the keypad, the K1 function state and
 * the coordinator's restore -- are stubbed and never reached. */
VFO_Info_t *gTxVfo;
bool  gUpdateDisplay;
EEPROM_Config_t gEeprom;    /* tx.c reads the BT PTT mode; the poller is not called */
void  rf_dual_reapply(void) { }
unsigned KEYBOARD_GetKey(void) { return 0u; }
void  FUNCTION_Select(int function) { (void)function; }
VfoState_t RADIO_CheckTx(const VFO_Info_t *vfo) { (void)vfo; return VFO_STATE_NORMAL; }
void  RADIO_SetVfoState(VfoState_t state) { (void)state; }
uint32_t systick_millis(void) { return 0u; }   /* the remote-PTT timeout */
void  audio_path_drive(int on) { (void)on; }   /* the 'w' variant's PC13 step */

/* ------------------------------------------------------------- bus stub --- */

#define MAX_XFER 64

struct xfer {
    int      is_read;               /* 0 = write, 1 = read */
    uint32_t cs;
    uint8_t  addr;
    uint8_t  data[MAX_XFER];
    unsigned len;                   /* bytes written (writes only) */
    uint16_t answer;                /* what a read returns */
};

static struct xfer log_[512];
static unsigned    log_len;
static int         bit_log[256];
static unsigned    bit_len;
static uint16_t    id_bk4829 = BK4829_ID;
static uint16_t    id_bk4815 = BK4815_ID;
static uint16_t    other_bk4829;
static uint16_t    other_bk4815;

static void log_reset(void)
{
    log_len = 0;
    bit_len = 0;
    memset(log_, 0, sizeof log_);
}

/* The manual-mode pieces bk4819.c uses for its frame fragments. */
void rf_bus_init(void) { }

void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }

void rf_bus_bit_out(int bit)
{
    if (bit_len < sizeof bit_log / sizeof bit_log[0])
        bit_log[bit_len++] = bit ? 1 : 0;
}

int rf_bus_bit_in(void) { return 0; }

void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    struct xfer *x = &log_[log_len++];

    x->is_read = 0;
    x->cs = cs;
    x->addr = addr;
    x->len = (len > MAX_XFER) ? MAX_XFER : len;
    memcpy(x->data, data, x->len);
}

uint16_t rf_bus_read(uint32_t cs, uint8_t addr)
{
    struct xfer *x = &log_[log_len++];
    uint16_t v;

    x->is_read = 1;
    x->cs = cs;
    x->addr = addr;

    if (cs == BK4829_CS_PIN)
        v = (addr & 0x7fu) == BK4829_REG_ID ? id_bk4829 : other_bk4829;
    else
        v = ((addr >> 1) & 0x7fu) == BK4815_REG_ID ? id_bk4815 : other_bk4815;

    x->answer = v;
    return v;
}

/* ---------------------------------------------------------------- checks --- */

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void check_hex(uint32_t got, uint32_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: got 0x%X, want 0x%X\n", what, got, want);
    } else {
        printf("  ok    %s (0x%X)\n", what, want);
    }
}

static int xfer_is(const struct xfer *x, uint32_t cs, uint8_t addr)
{
    return x->cs == cs && x->addr == addr;
}

/* The identity handshake: this is the "version" read the bring-up relies on. */
static void test_identity(void)
{
    printf("identity\n");

    log_reset();
    check(bk4829_detect(), "bk4829 detect true when reg 0 reads 0x4829");
    check(log_len == 1 && log_[0].is_read, "bk4829 detect is one read");

    log_reset();
    check(bk4815_detect(), "bk4815 detect true when reg 0 reads 0x4816");
    check(log_len == 1 && log_[0].is_read, "bk4815 detect is one read");

    id_bk4829 = 0x1111;
    log_reset();
    check(!bk4829_detect(), "bk4829 detect false on a wrong id");
    id_bk4829 = BK4829_ID;

    id_bk4815 = 0x2222;
    log_reset();
    check(!bk4815_detect(), "bk4815 detect false on a wrong id");
    id_bk4815 = BK4815_ID;
}

/* Framing: the two parts do not share an address encoding, and this is where a
 * copy-paste between them would show up. */
static void test_framing(void)
{
    printf("framing\n");

    log_reset();
    (void)bk4829_read_reg(0x00);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x80),
          "bk4829 read reg 0 sends 0x80 on PB8");

    log_reset();
    (void)bk4829_read_reg(0x33);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0xB3),
          "bk4829 read reg 0x33 sends 0x33|0x80");

    log_reset();
    bk4829_write_reg(0x7d, 0xE958);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x7D),
          "bk4829 write reg 0x7d sends 0x7d (no read flag)");
    check(log_[0].len == 2 && log_[0].data[0] == 0xE9 && log_[0].data[1] == 0x58,
          "bk4829 write sends the value MSB first");

    log_reset();
    (void)bk4815_read_reg(0x00);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x01),
          "bk4815 read reg 0 sends 0x01 on PB13");

    log_reset();
    (void)bk4815_read_reg(0x75);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0xEB),
          "bk4815 read reg 0x75 sends (0x75<<1)|1");

    log_reset();
    bk4815_write_reg(0x40, 0x8000);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x80),
          "bk4815 write reg 0x40 sends 0x40<<1");
    check(log_[0].len == 2 && log_[0].data[0] == 0x80 && log_[0].data[1] == 0x00,
          "bk4815 write sends the value MSB first");
}

/* The BK4829's boot sequence: the parts a wrong transcription would break. */
static void test_bk4829_config(void)
{
    unsigned i, writes;
    int saw_id_reset = 0, saw_7d = 0;
    uint16_t v7d = 0;

    printf("bk4829 boot configuration\n");

    log_reset();
    bk4829_configure();
    writes = bk4829_config_writes();

    check_hex(writes, 39, "39 register values");
    check(log_len == writes, "one transfer per table entry");
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x00) && log_[0].data[0] == 0x80,
          "first write is reg 0 = 0x8000 (the reset the stock starts with)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x00) && log_[1].data[0] == 0x00,
          "second write is reg 0 = 0x0000");
    check(xfer_is(&log_[writes - 1], BK4829_CS_PIN, 0x47),
          "last write is reg 0x47");

    for (i = 0; i < writes; i++) {
        if (log_[i].addr == 0x00 && log_[i].data[0] == 0x00 && log_[i].data[1] == 0x00)
            saw_id_reset++;
        if (log_[i].addr == 0x7d) {
            saw_7d++;
            v7d = (uint16_t)((log_[i].data[0] << 8) | log_[i].data[1]);
        }
    }
    check(saw_id_reset == 1, "reg 0 reset appears once after the 0x8000");
    check(saw_7d == 1, "reg 0x7d appears once (the tweak is folded in)");
    check_hex(v7d, 0xE958, "reg 0x7d carries the stock's effective value");
}

/* The BK4815's boot sequence: the block must be one select pulse, not 18. */
static void test_bk4815_config(void)
{
    unsigned block_len = 0, writes, i;
    const uint8_t *block = bk4815_config_block(&block_len);
    static const uint8_t expected[] = {
        0x6f, 0xc0, 0x0e, 0x3d, 0xb0, 0x41, 0xf7, 0x70, 0xf2, 0x74, 0x08, 0xf0,
        0xff, 0x33, 0xc3, 0xfa, 0xa2, 0xa3, 0x88, 0x00, 0x06, 0x03, 0x09, 0xfd,
        0x58, 0x17, 0x90, 0xa3, 0x88, 0xf9, 0x58, 0x00, 0x41, 0x5c, 0x08, 0xa0,
    };

    printf("bk4815 boot configuration\n");

    check_hex(block_len, sizeof expected, "the register-2 block is 36 bytes");
    check(memcmp(block, expected, sizeof expected) == 0,
          "the block matches the stock image at 0x08024E40");

    log_reset();
    bk4815_configure();
    writes = bk4815_config_writes();

    check_hex(writes, 33, "33 single writes after the block");
    check(log_len == writes + 1, "the block is one transfer, not 18");
    check(log_len > 1 && log_[0].len == 36 && xfer_is(&log_[0], BK4815_CS_PIN, 0x04),
          "the block goes out at address 2<<1 with all 36 bytes in one select");
    check(xfer_is(&log_[1], BK4815_CS_PIN, 0xE0) && log_[1].data[0] == 0xA0,
          "first single write is reg 0x70 = 0xa000");
    check(xfer_is(&log_[writes], BK4815_CS_PIN, 0x18),
          "last single write is reg 0x0c");

    for (i = 0; i < log_len; i++) {
        if (log_[i].cs != BK4815_CS_PIN) {
            check(0, "every transfer is on the BK4815 select");
            return;
        }
    }
    check(bk4815_ram_sourced_writes() == 3,
          "three writes are known to be placeholders (0x4c, 0x55, 0x62)");
}

/* The BK4815's synthesizer.  The stock's frequency word is at 0x71/0x72 (not
 * 0x38/0x39, and not 0x22); 0x22 is the SELCALL tone.  Expected word values
 * are the datasheet formula freq_Hz * Ndiv * 2^24 / 26e6 computed independently
 * of the driver. */
static void test_bk4815_synth(void)
{
    uint16_t reg4, freq_hi, freq_lo, tone;

    printf("bk4815 synthesizer (0x04 / 0x70-0x72 / 0x22)\n");

    /* VCO bands: the stock's FUN_0800EC70 thresholds, 187/270/383 MHz. */
    check_hex(bk4815_vco_band(14575000u), 3, "145.75 MHz is VCO band 3");
    check_hex(bk4815_vco_band(10800000u), 3, "108 MHz is VCO band 3");
    check_hex(bk4815_vco_band(17400000u), 3, "174 MHz is VCO band 3");
    check_hex(bk4815_vco_band(23000000u), 1, "230 MHz is VCO band 1");
    check_hex(bk4815_vco_band(32000000u), 2, "320 MHz is VCO band 2");
    check_hex(bk4815_vco_band(44600625u), 0, "446.00625 MHz is VCO band 0");
    check_hex(bk4815_vco_divider(0), 8, "band 0 divides by 8");
    check_hex(bk4815_vco_divider(1), 16, "band 1 divides by 16");
    check_hex(bk4815_vco_divider(2), 12, "band 2 divides by 12");
    check_hex(bk4815_vco_divider(3), 24, "band 3 divides by 24");

    check_hex(bk4815_frequency_word(14575000u, 3), 0x8689D89Du,
              "145.75 MHz band 3 word");
    check_hex(bk4815_frequency_word(44600625u, 0), 0x893B91B9u,
              "446.00625 MHz band 0 word");
    check_hex(bk4815_frequency_word(10800000u, 3), 0x63B13B13u,
              "108 MHz band 3 word");
    check_hex(bk4815_frequency_word(52000000u, 0), 0xA0000000u,
              "520 MHz band 0 word");

    /* 0x22 is a signalling tone, (freq/18466)*65536, not the RF word. */
    check_hex(bk4815_tone_word(1000u), 0x0DDDu, "1000 Hz tone word");
    check_hex(bk4815_tone_word(2000u), 0x1BBAu, "2000 Hz tone word");

    /* The full tune path, in FUN_0801703C's write order. */
    log_reset();
    bk4815_set_frequency(14575000u, false);
    check(log_len == 3, "set_frequency is three transfers");
    check(xfer_is(&log_[0], BK4815_CS_PIN, 0x08), "first is register 4");
    reg4 = (uint16_t)((log_[0].data[0] << 8) | log_[0].data[1]);
    check_hex(reg4, 0xB1C1u, "reg 4 = 0xb041 | band 3 << 7");
    check(xfer_is(&log_[1], BK4815_CS_PIN, 0xE0) && log_[1].len == 6,
          "then the 6-byte block at reg 0x70 in one select");
    check_hex(log_[1].data[0], 0xA0u, "reg 0x70 = 0xA000 (receive)");
    freq_hi = (uint16_t)((log_[1].data[2] << 8) | log_[1].data[3]);
    freq_lo = (uint16_t)((log_[1].data[4] << 8) | log_[1].data[5]);
    check_hex(freq_hi, 0x8689u, "reg 0x71 = high word");
    check_hex(freq_lo, 0xD89Du, "reg 0x72 = low word");
    check(xfer_is(&log_[2], BK4815_CS_PIN, 0xFC) && log_[2].len == 4,
          "last is the 4-byte calibration block at reg 0x7e");

    log_reset();
    bk4815_set_frequency(44600625u, true);
    check(log_len == 3 && xfer_is(&log_[0], BK4815_CS_PIN, 0x08),
          "446.00625 MHz tune: reg 4 first");
    check_hex((uint16_t)((log_[0].data[0] << 8) | log_[0].data[1]), 0xB041u,
              "reg 4 = 0xb041 (band 0)");
    check_hex(log_[1].data[0], 0xE0u, "reg 0x70 = 0xE000 (transmit)");
    check_hex((uint16_t)((log_[1].data[2] << 8) | log_[1].data[3]), 0x893Bu,
              "446.00625 MHz reg 0x71");
    check_hex((uint16_t)((log_[1].data[4] << 8) | log_[1].data[5]), 0x91B9u,
              "446.00625 MHz reg 0x72");

    /* 0x7E/0x7F: in every VCO band the word is the same -137 kHz (the IF) in
     * that band's units, Hz = word * 26 MHz / (Ndiv * 2^24) -- the reason
     * bit 10 of 0x75 must select low-side injection (radio 2026-10-30). */
    {
        static const uint32_t band_freq[4] = { 44600625u, 20000000u, 30000000u, 14575000u };
        uint8_t b;

        for (b = 0; b < 4u; b++) {
            int32_t word;
            int64_t hz;

            log_reset();
            bk4815_set_frequency(band_freq[b], false);
            word = (int32_t)(((uint32_t)log_[2].data[0] << 24) | ((uint32_t)log_[2].data[1] << 16) |
                             ((uint32_t)log_[2].data[2] << 8) | log_[2].data[3]);
            hz = ((int64_t)word * 26000000) /
                 ((int64_t)bk4815_vco_divider(bk4815_vco_band(band_freq[b])) << 24);
            check(hz > -137050 && hz < -136950, "0x7E/0x7F is the -137 kHz IF in each band");
        }
    }

    /* The band register keeps bit 10 set: low-side injection (LO + IF = f). */
    other_bk4815 = 0x0000u;
    log_reset();
    bk4815_set_band(0x0Au);
    check(log_len == 2 && !log_[1].is_read && xfer_is(&log_[1], BK4815_CS_PIN, 0xEA),
          "set_band reads then writes reg 0x75");
    check_hex((uint16_t)((log_[1].data[0] << 8) | log_[1].data[1]), 0x040Au,
              "0x75 = band 0x0A with bit 10 (low-side) set");

    log_reset();
    bk4815_set_tone(1000u);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x44),
          "set_tone writes reg 0x22 (0x22<<1)");
    tone = (uint16_t)((log_[0].data[0] << 8) | log_[0].data[1]);
    check_hex(tone, 0x0DDDu, "reg 0x22 = the 1000 Hz tone word");

    /* RSSI: register 0x44, bits 6:0. */
    other_bk4815 = 0x00D5u;
    log_reset();
    check_hex(bk4815_read_rssi(), 0x55u, "RSSI is reg 0x44 masked to 7 bits");
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x89),
          "RSSI read addresses reg 0x44 ((0x44<<1)|1)");
    other_bk4815 = 0u;

    /* AF source: register 0x49, 0x9A02 on / 0x1A02 off (the stock's
     * FUN_08015F48 / FUN_0801CCE8). */
    log_reset();
    bk4815_set_af(true);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x92),
          "AF on addresses reg 0x49 (0x49<<1)");
    check_hex((uint16_t)((log_[0].data[0] << 8) | log_[0].data[1]), 0x1A02u,
              "AF on = 0x1A02 (hard mute clear)");
    log_reset();
    bk4815_set_af(false);
    check_hex((uint16_t)((log_[0].data[0] << 8) | log_[0].data[1]), 0x9A02u,
              "AF off = 0x9A02 (hard mute set)");
}

/* The accessors the console's read-back verification walks.  If these are wrong
 * the verification lies, so they get their own check. */
static void test_accessors(void)
{
    uint8_t reg = 0;
    uint16_t value = 0;
    unsigned n;

    printf("table accessors\n");

    n = bk4829_config_writes();
    bk4829_config_entry(0, &reg, &value);
    check_hex(reg, 0x00, "bk4829 entry 0 reg");
    check_hex(value, 0x8000, "bk4829 entry 0 value");

    bk4829_config_entry(n - 1, &reg, &value);
    check_hex(reg, 0x47, "bk4829 last entry reg");
    check_hex(value, 0x6042, "bk4829 last entry value");

    reg = 0xFF;
    value = 0xFFFF;
    bk4829_config_entry(n, &reg, &value);          /* out of range: untouched */
    check_hex(reg, 0xFF, "bk4829 out-of-range entry leaves reg alone");
    check_hex(value, 0xFFFF, "bk4829 out-of-range entry leaves value alone");

    n = bk4815_config_writes();
    bk4815_config_entry(0, &reg, &value);
    check_hex(reg, 0x70, "bk4815 entry 0 reg");
    check_hex(value, 0xA000, "bk4815 entry 0 value");
    bk4815_config_entry(n - 1, &reg, &value);
    check_hex(reg, 0x0C, "bk4815 last entry reg");
    check_hex(value, 0x0A03, "bk4815 last entry value");
}

/* The K1-compatible layer: same bus, the K1's register sequences. */
static void test_k1_interface(void)
{
    unsigned i;
    static const int expect_a5[8] = { 1, 0, 1, 0, 0, 1, 0, 1 };

    printf("k1-compatible bk4819 layer\n");

    /* Its init must start with the same register-0 reset pair the stock uses. */
    log_reset();
    BK4819_Init();
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x00) && log_[0].data[0] == 0x80,
          "BK4819_Init starts with reg 0 = 0x8000 (as the stock does)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x00) && log_[1].data[0] == 0x00,
          "and then reg 0 = 0x0000");

    /* 145.7500 MHz in 10 Hz units, split across 0x38/0x39 exactly as the stock
     * writes it (FUN_08017158 does the same with no scaling). */
    log_reset();
    BK4819_SetFrequency(14575000u);
    check(log_len == 2, "SetFrequency is two register writes");
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x38) && log_[0].data[0] == 0x65 &&
          log_[0].data[1] == 0x98, "reg 0x38 = 0x6598 (low word)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x39) && log_[1].data[0] == 0x00 &&
          log_[1].data[1] == 0xDE, "reg 0x39 = 0x00DE (high word)");

    /* RSSI is the stock's register too. */
    other_bk4829 = 0x7BCD;
    log_reset();
    check_hex(BK4819_GetRSSI(), 0x7BCD & 0x01FF, "GetRSSI masks reg 0x67 to 9 bits");
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0xE7),
          "GetRSSI reads reg 0x67 with the read flag");

    /* The 0x30/0x47 write caches must suppress a repeated write. */
    log_reset();
    BK4819_WriteRegister(BK4819_REG_30, 0x1234);
    BK4819_WriteRegister(BK4819_REG_30, 0x1234);
    check(log_len == 1, "a repeated reg 0x30 write is cached away");
    log_reset();
    BK4819_WriteRegister(BK4819_REG_47, 0x5678);
    BK4819_WriteRegister(BK4819_REG_47, 0x9999);
    check(log_len == 2, "a changed reg 0x47 write is not");

    /* Frame fragment: MSB first, and only the bits. */
    log_reset();
    BK4819_WriteU8(0xA5);
    check(bit_len == 8, "WriteU8 shifts 8 bits");
    for (i = 0; i < 8; i++)
        if (bit_log[i] != expect_a5[i])
            break;
    check(i == 8, "WriteU8 sends 0xA5 MSB first");

    /* The gEeprom replacement: gains come from the driver's own setter. */
    log_reset();
    BK4819_SetRxAudioGains(5, 3);
    BK4819_SetRxAudioGain();
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x48),
          "SetRxAudioGain writes reg 0x48");
    check(log_[0].data[0] == 0xB0 && log_[0].data[1] == 0x53,
          "with the driver's gains packed in ((11<<12)|(5<<4)|3)");
}

/* Record the last BK4829 write to 0x33. */
static uint16_t last_reg33(void)
{
    unsigned i;

    for (i = log_len; i-- > 0; )
        if (xfer_is(&log_[i], BK4829_CS_PIN, 0x33))
            return (uint16_t)((log_[i].data[0] << 8) | log_[i].data[1]);
    return 0xFFFFu;
}

static void test_pa_rx_path(void)
{
    printf("pa receive path (reg 0x33)\n");

    /* The lane decides the receive path (stock: pin 4 for the BK4815 entry).
     * The K1's pin 0 ("RX_ENABLE") is cleared on either lane, as the stock's
     * receive entry clears pins 0/1 (FUN_080093EC -> FUN_080137D4(3, 0)): it
     * is the VHF transmit band path, and set on receive it cost the BK4829
     * ~45-57 dB at VHF (radio 2026-10-30).  The 0x9000 bits BK4819_Init() set
     * must survive, except the BT pin's paired 0x1000, which the stock's
     * BT-pin-off update clears. */
    pa_set_rx_lane(false);
    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(14550000u);
    check_hex(last_reg33(), 0x8000, "BK4829 lane clears pin 0, pin 4 and the BT pair bit");

    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(44600625u);
    check_hex(last_reg33(), 0x8000, "BK4829 lane at UHF is the same path state");

    pa_set_rx_lane(true);
    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(44600625u);
    check_hex(last_reg33(), 0x8004, "BK4815 lane sets pin 4 and clears the BK4829's pin 0");

    pa_set_rx_lane(false);
    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(11800000u);      /* 118.0 MHz */
    check_hex(last_reg33(), 0x8000, "BK4829 lane at 118 MHz is the same path state");
}

/* The K1's power ladder reaches 0x36 (its SetupPowerAmplifier) and, with the
 * stock's arithmetic, the PB14 bias PWM compare. */
static void test_pa_tx_path(void)
{
    printf("pa transmit path (0x33/0x36/PWM)\n");

    /* 145.5000 MHz: VHF, so the K1's VHF PA gain (0x08), not the UHF one. */
    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(14550000u);
    pa_tx_enable(0x40u);
    check_hex(pa_last_reg33(), 0x8042u, "VHF TX: stock base + band pin + T/R; BT pin off");
    check_hex(pa_last_reg36(), 0x4088u, "VHF TX: 0x36 = (0x40<<8) | PA-CTL | gain 0x08");
    check_hex(pa_last_compare(), (0x40u * PA_PWM_ARR) / 255u,
              "VHF TX: PWM compare = power*ARR/255");

    /* 446.00625 MHz: UHF, the K1's UHF gain (0x22). */
    log_reset();
    BK4819_Init();
    log_reset();
    pa_select_band(44600625u);
    pa_tx_enable(0xFFu);
    check_hex(pa_last_reg33(), 0x8022u, "UHF TX: stock base + band pin + T/R; BT pin off");
    check_hex(pa_last_reg36(), 0xFFA2u, "UHF TX: 0x36 = (0xFF<<8) | PA-CTL | gain 0x22");
    check_hex(pa_last_compare(), PA_PWM_MAX_DUTY, "UHF TX: full power clamps the compare");
}

/* The last value the bus saw written to a BK4829 register, 0xFFFF if none. */
static uint16_t last_reg(uint8_t addr)
{
    unsigned i;

    for (i = log_len; i-- > 0; )
        if (xfer_is(&log_[i], BK4829_CS_PIN, addr))
            return (uint16_t)((log_[i].data[0] << 8) | log_[i].data[1]);
    return 0xFFFFu;
}

static uint16_t last_bk4815_reg70(void)
{
    unsigned i;

    for (i = log_len; i-- > 0; )
        if (!log_[i].is_read && xfer_is(&log_[i], BK4815_CS_PIN, 0xE0) &&
            log_[i].len >= 2u)
            return (uint16_t)((log_[i].data[0] << 8) | log_[i].data[1]);
    return 0xFFFFu;
}

/* The stock's FUN_080137D4(mask=4, value=4) drives GPIO2 by clearing both
 * 0x0010 (output) and its paired 0x1000 configuration bit, then setting the
 * output when requested.  The port must do this without replacing the rest of
 * the PA/band register.  The lane still sets pin 0 for a BK4829 receive, so the
 * base here is 0x8040 (the BT-off update has already cleared 0x1000). */
static void test_pa_bt_audio(void)
{
    printf("pa BT audio GPIO (stock 0x33 paired bit)\n");

    log_reset();
    BK4819_Init();
    pa_select_band(14550000u);
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
    pa_set_bt_audio(true);
    check(pa_bt_audio(), "BT link state retained");
    check_hex(last_reg33(), 0x8050u, "BT on keeps pin 0, sets output 0x0010");

    log_reset();
    pa_tx_enable(0x40u);
    check_hex(pa_last_reg33(), 0x8052u, "VHF TX preserves BT output and stock paired-bit clear");

    /* rx_service() keeps running while transmitting: the receive-path calls it
     * makes every 10 ms must leave the TX band pin alone (radio 2026-10-31:
     * pin 0 cleared a slice after PTT, 0x33 = 0x8003, nothing transmitted). */
    log_reset();
    pa_set_rx_lane(false);
    pa_select_band(14550000u);
    check(last_reg33() == 0xFFFFu || (last_reg33() & 0x0040u) != 0u,
          "no receive-path 0x33 write drops the VHF TX band pin during TX");
    check(BK4819_IsGpioOutSet(BK4819_GPIO0_PIN28_RX_ENABLE),
          "the 0x33 shadow still holds the VHF TX band pin while transmitting");

    log_reset();
    pa_set_bt_audio(false);
    check(!pa_bt_audio(), "BT disconnect state retained");
    check_hex(last_reg33(), 0x8042u, "BT off clears output, keeps band/T-R and paired-bit state");

    pa_set_bt_audio(true);
    pa_rx_enable();
    check_hex(pa_last_reg33(), 0x8010u, "VHF RX transition preserves BT output, clears pin 0");
    pa_select_band(44600625u);
    check_hex(pa_last_reg33(), 0x8010u, "UHF RX band selection preserves BT output");
    pa_tx_enable(0xFFu);
    check_hex(pa_last_reg33(), 0x8032u, "UHF TX preserves BT output and applies band/T-R pins");
    pa_rx_enable();
    check_hex(pa_last_reg33(), 0x8010u, "UHF TX-to-RX preserves BT output");
    pa_set_bt_audio(false);
    check_hex(pa_last_reg33(), 0x8000u, "BT off after RX clears only the BT output");
}

/* The order inside `tx_start()`: the PA enable (`0x36`, PA-CTL + bias) must be
 * written **after** `BK4819_PrepareTransmit()`, whose `BK4819_TxOn_Beep()`
 * writes `0x36 = 0`.  With the two the other way round the PA is disabled for
 * the whole transmission: the chip's own low-level carrier still radiates (a
 * nearby receiver hears it) but there is no measurable power at the antenna.
 * The bench that validated the transmit chain wrote `0x36` after this call
 * (docs/ra89r_rfpath.md), and the driver extraction reversed it -- this is the
 * regression guard. */
static void test_tx_order(void)
{
    printf("tx_start register order (PA enable after PrepareTransmit)\n");

    log_reset();
    BK4819_Init();
    log_reset();
    tx_start(14550000u, 0x40u, TX_SOURCE_MIC);
    check_hex(last_reg(0x36), 0x4088u, "0x36 keeps PA-CTL + bias after PrepareTransmit");
    check_hex(last_reg(0x7D), TX_REG7D_POWER, "0x7D = the stock's power/bias value");
    check_hex(last_reg(0x37), 0x9D1F, "0x37 = the stock's TX value");
    check_hex(last_reg(0x50), TX_REG50_UNMUTE, "0x50 = the stock's TX unmute");
    tx_stop();
    check_hex(last_bk4815_reg70(), 0xA000u,
              "BK4815 0x70 restored from TX E000 to RX A000 after tx_stop");
}

/* A remote (UART) transmission is not cancelled by the PTT poller: it used
 * to see "PTT not pressed" while tx_active() and stop it within 10 ms. */
static void test_tx_remote(void)
{
    static VFO_Info_t vfo;
    unsigned i;

    printf("remote PTT survives the PTT poller\n");
    vfo.freq_config_TX.Frequency = 14550000u;
    vfo.pTX = &vfo.freq_config_TX;
    gTxVfo = &vfo;

    check(tx_remote_ptt(true) == 0u && tx_active() && tx_remote_active(),
          "tx_remote_ptt(true) keys the measured chain");
    for (i = 0; i < 5u; i++)
        tx_poll_ptt();                  /* no PTT key down (stub) */
    check(tx_active() && tx_remote_active(), "five poller passes leave it keyed");
    (void)tx_remote_ptt(false);
    check(!tx_active() && !tx_remote_active(), "tx_remote_ptt(false) releases it");
    gTxVfo = 0;
}

/* The airband is never transmitted on, whoever calls tx_start(). */
static void test_tx_airband(void)
{
    static const uint32_t freqs[] = { 10800000u, 12010000u, 13699000u };
    unsigned i;

    printf("tx_start refuses the airband (108-137 MHz)\n");
    for (i = 0; i < sizeof freqs / sizeof freqs[0]; i++) {
        log_reset();
        tx_start(freqs[i], 0x40u, TX_SOURCE_MIC);
        check(!tx_active() && log_len == 0u, "no TX and no bus traffic on the airband");
    }
    log_reset();
    tx_start(13700000u, 0x40u, TX_SOURCE_MIC);
    check(tx_active(), "137.000 MHz (just above the airband) still keys");
    tx_stop();
}

int main(void)
{
    printf("rf register-layer test (stub bus, no radio)\n\n");

    test_identity();
    test_framing();
    test_bk4829_config();
    test_bk4815_config();
    test_bk4815_synth();
    test_accessors();
    test_k1_interface();
    test_pa_rx_path();
    test_pa_tx_path();
    test_pa_bt_audio();
    test_tx_order();
    test_tx_airband();
    test_tx_remote();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
