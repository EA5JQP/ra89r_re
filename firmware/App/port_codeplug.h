/* The RA89R codeplug: the stock's own flat layout in the external SPI NOR,
 * decoded for the K1 application (see ra89r_codeplug.md).
 *
 * The port keeps the K1's RAM structures (gEeprom, VFO_Info_t) and its
 * SETTINGS_* interface, but the *bytes on the chip* are the stock's, not the
 * K1's.  Everything that knows an RA89R offset lives here, so the imported K1
 * code never learns about them.
 *
 * Compatibility rule (ra89r_port.md): the stock's regions are read, never
 * written, except the channel record a user explicitly edits -- and that is not
 * implemented yet, so today this module is read-only.
 */
#ifndef PORT_CODEPLUG_H
#define PORT_CODEPLUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

/* Where each table sits in the first 16 KB of the chip.  These are the CPS's
 * own constants (Class1.cs) verified against the dump; ra89r_codeplug.md has
 * the evidence for every one. */
#define RA89R_CP_CODEPLUG_SIZE   0x4000u

#define RA89R_CP_RECORD_BASE     0x0000u   /* 21-byte channel records */
#define RA89R_CP_RECORD_STRIDE   21u
#define RA89R_CP_RECORD_COUNT    210u      /* slots the image has room for */
#define RA89R_CP_NAME_BASE       4416u     /* 10-byte extended channel names */
#define RA89R_CP_NAME_STRIDE     10u
#define RA89R_CP_ENABLE_BASE     7936u     /* 32-byte "channel used" bitmap */
#define RA89R_CP_SKIP_BASE       7968u     /* 32-byte "scan allow" bitmap */
#define RA89R_CP_BITMAP_BYTES    32u
#define RA89R_CP_BAND_BASE       8000u     /* band-range table */
#define RA89R_CP_OPEN_NAME_BASE  8048u     /* radio name, 16 chars */
#define RA89R_CP_FREQ_CODE_BASE  8080u
#define RA89R_CP_MODEL_NAME_BASE 8096u     /* model name, 16 chars */
#define RA89R_CP_CAL_BASE        8208u
#define RA89R_CP_SETTINGS_BASE   8224u     /* the stock's general settings */
#define RA89R_CP_SIGNATURE_BASE  0x3FF0u   /* six bytes, written by the stock */

/* One channel record, exactly 21 bytes.  The layout is the CPS's encoder
 * output (ChinfDetail.ChgChStringPro) truncated to ConOneChDatCt*2 hex chars:
 *
 *   [0]  u32 RxFrequency, 10 Hz units
 *   [4]  u32 TxFrequency, 10 Hz units
 *   [8]  u16 RxTone
 *   [10] u16 TxTone
 *   [12] flags A: bit0 TX inhibit, bit1 frequency reverse, bits2-3 busy lock,
 *        bits4-5 bandwidth, bits6-7 TX power
 *   [13] flags B: bit0 GPS/call id, bit1 talk around, bit2 name present,
 *        bits3-4 optional signalling, bits5-6 squelch kind
 *   [14] flags C: bits0-3 step, bits4-5 DTMF PTT id, bits6-7 5-tone PTT id
 *   [15] char Name[6]
 */
typedef struct {
    uint32_t rx_frequency;
    uint32_t tx_frequency;
    uint16_t rx_tone;
    uint16_t tx_tone;
    uint8_t  flags_a;
    uint8_t  flags_b;
    uint8_t  flags_c;
    char     name[6];
} __attribute__((packed)) ra89r_codeplug_record_t;

/* Flags A/B/C, named so the callers do not carry the bit numbers around.
 * The label each bit carries in the CPS's channel dialog is in
 * ra89r_codeplug.md; the two the port maps are TX inhibit and frequency
 * reverse, which are exactly the K1's TX_LOCK and FrequencyReverse. */
#define RA89R_CP_FLAGA_TX_INHIBIT 0x01u
#define RA89R_CP_FLAGA_FREQ_REV   0x02u
#define RA89R_CP_FLAGA_BUSY_MASK  0x0Cu
#define RA89R_CP_FLAGA_BW_MASK    0x30u
#define RA89R_CP_FLAGA_POWER_MASK 0xC0u
#define RA89R_CP_FLAGB_GPS        0x01u
#define RA89R_CP_FLAGB_TALKAROUND 0x02u
#define RA89R_CP_FLAGB_NAME       0x04u
#define RA89R_CP_FLAGB_SIG_MASK   0x18u
#define RA89R_CP_FLAGB_SQL_MASK   0x60u
#define RA89R_CP_FLAGC_STEP_MASK  0x0Fu

/* Call after the external-NOR driver is up, before anything reads the codeplug.
 * It drops the cached bitmaps, so a read taken while the bus was dead cannot
 * masquerade as "this radio has no channels". */
void port_codeplug_init(void);

/* Read one record.  False when the channel has no room in the image. */
bool port_codeplug_read(uint16_t channel, ra89r_codeplug_record_t *record);

/* The two bitmaps.  `used` is the CPS's channel-enable map, `excluded` the
 * inverse of its "scan allow" map: both are cached after the first read. */
bool port_codeplug_used(uint16_t channel);
bool port_codeplug_excluded(uint16_t channel);

/* Decode a record into the K1's channel description.  False for a channel that
 * is not usable (out of range, disabled, or with no frequency). */
bool port_codeplug_scan_info(uint16_t channel, ChannelScanDisplayInfo_t *info);

/* The K1's channel name, from the record and the extended-name table. */
void port_codeplug_name(char *out, size_t size, uint16_t channel);

/* The K1's ChannelAttributes_t, as a plain u16 (misc.c keeps the bitfield).
 * 0xFFFF marks a channel the radio does not have. */
uint16_t port_codeplug_attributes(uint16_t channel);
void     port_codeplug_save_attributes(uint16_t channel, uint16_t value);

/* The frequency (VFO) channels have no home in the stock codeplug -- the stock
 * keeps its VFO state in RAM and its own settings block -- so the port keeps
 * them in RAM and in its own settings blob.  `band` is a FREQUENCY_Band_t. */
bool port_codeplug_freq_get(uint16_t channel, uint8_t vfo, ChannelScanDisplayInfo_t *info);
void port_codeplug_freq_set(uint16_t channel, uint8_t vfo, const ChannelScanDisplayInfo_t *info);
void port_codeplug_freq_snapshot(uint8_t *dest, size_t size);
bool port_codeplug_freq_restore(const uint8_t *src, size_t size);

/* The stock's shared settings, read (never written) from the settings block so
 * a radio configured with the stock firmware or its CPS looks the same here. */
void port_codeplug_shared_settings(void);

#endif
