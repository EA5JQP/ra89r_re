/* Bluetooth debug console -- `!`-prefixed lines on the radio's UART console.
 *
 * The console's single-letter commands are all taken, so the BT debug
 * commands are whole lines that start with '!' and end with CR or LF.  Every
 * reply starts with `BT <ms> <kind> `, so a host tool (tools/ra89r_bt.py) can
 * pick its lines out of whatever else the console prints:
 *
 *   !s           status      BT <ms> S en=1 state=Linked link=1 sco=1 ...
 *   !t1 / !t0    trace on/off: every module line, its parser event, and every
 *                PTT / TX transition, as they happen
 *   !a<text>     send <text> + CRLF to the module (raw AT command)
 *   !i<text>     inject <text> as if the module had sent it (parser + service)
 *   !r           restart the module (PD0 reset, the stock config sequence)
 *   !l           hex dump of the last bytes received from the module
 *   !?           list these commands
 *
 * Trace lines:  BT <ms> < <line> [<EVENT>]   received (EVENT NONE = unparsed)
 *               BT <ms> > <line>             sent to the module
 *               BT <ms> P headset=. radio=. type=. allow=. tx=.
 *
 * Device-header free: the radio-side read-outs come in through
 * `bt_debug_io_t`, so the core is host-tested (tools/test_bt_debug.c).  See
 * docs/ra89r_bt_debug.md.
 */
#ifndef APP_BT_DEBUG_H
#define APP_BT_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void     (*out)(const char *text);  /* console output (required) */
    uint32_t (*millis)(void);           /* timestamp, or NULL for 0 */
    bool     (*radio_ptt)(void);        /* the radio's own PTT key */
    bool     (*tx_active)(void);        /* the measured TX chain is keyed */
    uint8_t  (*ptt_type)(void);         /* gEeprom.BT_PTTType: 0 BT, 1 radio, 2 both */
    unsigned (*rx_bytes)(void);         /* bytes received from the module */
    unsigned (*rx_log)(uint8_t *out, unsigned cap); /* last received bytes */
    void     (*restart)(void);          /* re-run the module bring-up */
} bt_debug_io_t;

void bt_debug_init(const bt_debug_io_t *io);

/* True while a '!' line is being collected: the console must route its
 * characters here instead of to the single-letter switch. */
bool bt_debug_active(void);
/* One console character: '!' starts a line, CR/LF runs it, ESC drops it. */
void bt_debug_feed(char c);
/* Call on a 10 ms slice: reports PTT/TX transitions while tracing and drops a
 * '!' line left unterminated for more than 5 s. */
void bt_debug_tick(void);
bool bt_debug_tracing(void);

#endif /* APP_BT_DEBUG_H */
