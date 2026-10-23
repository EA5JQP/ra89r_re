/* BK1080 FM receiver -- register framing, the stock's init block, tuning and
 * the seek/status path.
 *
 * The part is a 65-108 MHz FM receiver (docs/BK1080.pdf) on its own two-wire
 * bus, `i2c_bus.c`:
 *
 *   PC14  clock  (`BK1080_SCL_PIN`)
 *   PB2   data   (`BK1080_SDA_PIN`, output to send, released to read)
 *
 * The bus was once read as a companion battery gauge; docs/ra89r_bk1080.md has
 * the correction and the evidence.  Its framing is unusual for I2C -- the
 * device id is a fixed byte and the read flag rides in the register control
 * word:
 *
 *     start, 0x80, (reg & 0x7f) << 1 | R/W, then 16-bit words MSB first
 *
 * which is byte for byte what the stock's `FUN_08007034` (address phase),
 * `FUN_0800705C` (a byte, then the ACK poll), `FUN_08006E78` (a byte in),
 * `FUN_08006EF0`/`FUN_08006F4C` (start/stop), `FUN_08007158` (read words) and
 * `FUN_08007240`/`FUN_08007284` (write bytes / a 16-bit value) do.
 *
 * The stock never checks an identity for this part (unlike the BK4829/BK4815);
 * it reads register 0x01 nowhere, so `bk1080_read_id()` is offered as a liveness
 * probe with the datasheet's expected value, not as something the stock does.
 *
 * None of this has run on the radio.  The evidence is the stock image and the
 * datasheet; see docs/ra89r_bk1080.md for the function-by-function derivation.
 *
 * This header also carries the K1/F4HWN driver API (`BK1080_Init`,
 * `BK1080_SetFrequency`, ...), imported from the UV-K1/K5V3 project's
 * `App/driver/bk1080.h` + `bk1080-regs.h` so the ported `app/fm.c` and
 * `ui/fmradio.c` link unchanged.  The K1's implementation lives in this file's
 * `.c` (see NOTICE); it is the same part and the same wire framing, with a
 * different vendor register image (the K1's 33-entry table vs the RA89R stock's
 * 68-byte block) and the K1's 100 kHz frequency unit instead of the stock
 * path's 10 Hz.  Both are offered because the stock path is what the console
 * `j` bench and `tools/test_bk1080.c` exercise; the FM feature uses the K1 API.
 */
#ifndef DRIVER_BK1080_H
#define DRIVER_BK1080_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/bk1080-regs.h"

/* The fixed device-id byte the stock clocks out before every control word. */
#define BK1080_ADDR  0x80u

/* Public register map (docs/BK1080.pdf table 11). */
#define BK1080_REG_ID       0x01u   /* chip id, reads 0x1080 (read-only) */
#define BK1080_REG_POWER    0x02u   /* ENABLE/DISABLE, SEEK/SEEKUP/SKMODE, ... */
#define BK1080_REG_CHANNEL  0x03u   /* TUNE (bit 15) + CHAN[9:0] */
#define BK1080_REG_SYSCFG1  0x04u   /* de-emphasis, AGC, GPIO, blend */
#define BK1080_REG_SYSCFG2  0x05u   /* SEEKTH, BAND, SPACE, VOLUME */
#define BK1080_REG_SYSCFG3  0x06u   /* soft mute, seek SNR/impulse thresholds */
#define BK1080_REG_TEST1    0x07u   /* FREQD[11:0] + SNR[3:0] (read back) */
#define BK1080_REG_RSSI     0x0au   /* STC, SF/BL, AFCRL, STEN, ST, RSSI[7:0] */
#define BK1080_REG_READCHAN 0x0bu   /* IMPC[3:0] + READCHAN[9:0] */

#define BK1080_ID           0x1080u

/* Register 0x02 fields. */
#define BK1080_POWER_ENABLE   (1u << 0)
#define BK1080_POWER_DISABLE  (1u << 6)
#define BK1080_POWER_SEEK     (1u << 8)
#define BK1080_POWER_SEEKUP   (1u << 9)
#define BK1080_POWER_SKMODE   (1u << 10)

/* Register 0x03 field. */
#define BK1080_CHANNEL_TUNE   (1u << 15)

/* Register 0x05 fields. */
#define BK1080_BAND_SHIFT     6u
#define BK1080_BAND_MASK      (3u << BK1080_BAND_SHIFT)
#define BK1080_SPACE_SHIFT    4u
#define BK1080_SPACE_MASK     (3u << BK1080_SPACE_SHIFT)
#define BK1080_VOLUME_MASK    0x000fu
#define BK1080_SEEKTH_SHIFT   8u

/* Register 0x0a fields. */
#define BK1080_STATUS_STC     (1u << 14)   /* seek/tune complete */
#define BK1080_STATUS_SFBL    (1u << 13)   /* seek fail / band limit */
#define BK1080_STATUS_AFCRL   (1u << 12)   /* AFC railed (invalid channel) */
#define BK1080_STATUS_STEN    (1u << 9)    /* stereo decoder indicator */
#define BK1080_STATUS_ST      (1u << 8)    /* stereo indicator */
#define BK1080_STATUS_RSSI_MASK 0x00ffu

/* Bring up the bus pins. */
void bk1080_init(void);

/* One register access.  Reads/writes one 16-bit word MSB first. */
uint16_t bk1080_read_reg(uint8_t reg);
void     bk1080_write_reg(uint8_t reg, uint16_t value);

/* Write `len` bytes from one register address; the part auto-increments, which
 * is how the stock loads its 68-byte init block. */
void bk1080_write_block(uint8_t reg, const uint8_t *bytes, unsigned len);

/* The datasheet chip id (register 0x01), for a caller that wants a liveness
 * check.  The stock itself never reads it. */
uint16_t bk1080_read_id(void);

/* Replay the stock's power-up configuration, `FUN_08007124`: the 68-byte block
 * into register 0, then register 0x32 = 0x285c and 0x28dc (the stock waits ~100
 * delay loops between them; see the note in bk1080.c).  The block's own
 * register 0x02 carries ENABLE = 1. */
void bk1080_configure(void);

/* The 68-byte block, for the console/test to compare against the image. */
const uint8_t *bk1080_config_block(unsigned *len);

/* The two register-0x32 writes `bk1080_configure()` sends after the block. */
unsigned bk1080_config_reg32_writes(void);
void     bk1080_config_reg32_entry(unsigned i, uint16_t *value);

/* Tune to `freq_10hz` (the firmware's 10 Hz units), `FUN_08006F9C`: pick BAND
 * from the frequency, write SEEKTH/BAND/SPACE/VOLUME to register 5, then CHAN
 * and TUNE|CHAN to register 3.  The channel step is 100 kHz. */
void bk1080_set_frequency(uint32_t freq_10hz);

/* Wait for the tune to complete: poll STC up to `timeout` times.  The stock
 * polls the status register rather than delaying, and the part needs a moment
 * after TUNE before STC and READCHAN are valid -- reading READCHAN immediately
 * returns the band base.  Returns true when STC came back. */
bool bk1080_wait_tune(unsigned timeout);

/* The tuned frequency in 10 Hz units, `FUN_0800687C`: READCHAN[9:0] from
 * register 0x0b plus the band's base (875/760/640 in 100 kHz units). */
uint32_t bk1080_get_frequency(void);

/* Register 0x0a: the raw RSSI/status word, and the fields the stock uses. */
uint16_t bk1080_read_status(void);
uint8_t  bk1080_get_rssi(void);          /* RSSI[7:0], dBuV */
bool     bk1080_seek_complete(void);     /* STC  */
bool     bk1080_seek_failed(void);       /* SF/BL */
uint8_t  bk1080_get_snr(void);           /* SNR[3:0] from register 0x07 */

/* Seek controls, matching the stock's read-modify-writes. */
void bk1080_seek_up(void);     /* set SEEK | SEEKUP | SKMODE  (`FUN_08006952`) */
void bk1080_clear_seek(void);  /* clear SEEK                  (`FUN_08006982`) */
void bk1080_clear_tune(void);  /* clear TUNE                  (`FUN_080069A6`) */

/* ---------------------------------------------------------------------------
 * The K1/F4HWN driver API (imported -- see the header comment and NOTICE).
 *
 * Frequencies here are the K1's 100 kHz units (875 = 87.5 MHz), not the stock
 * path's 10 Hz.  The register access is the same framing above; only the init
 * image and the arithmetic differ.
 * ------------------------------------------------------------------------- */

extern uint16_t BK1080_BaseFrequency;
extern uint16_t BK1080_FrequencyDeviation;

void     BK1080_Init0(void);
void     BK1080_Init(uint16_t Frequency, uint8_t band);
uint16_t BK1080_ReadRegister(BK1080_Register_t Register);
void     BK1080_WriteRegister(BK1080_Register_t Register, uint16_t Value);
void     BK1080_Mute(bool Mute);
uint16_t BK1080_GetFreqLoLimit(uint8_t band);
uint16_t BK1080_GetFreqHiLimit(uint8_t band);
void     BK1080_SetFrequency(uint16_t frequency, uint8_t band);
void     BK1080_GetFrequencyDeviation(uint16_t Frequency);

#endif /* DRIVER_BK1080_H */
