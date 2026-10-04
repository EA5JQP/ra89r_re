/* Bluetooth service -- the YBT100 state machine and command queue.
 *
 * Device-header free: the service talks to the module only through
 * `driver/bluetooth.h`, whose USART3 half is compiled out under
 * BLUETOOTH_HOST_TEST, so the core is host-testable.  The application owns the
 * settings (`gEeprom.BT_*`) and drives the service with these calls.  See
 * docs/ra89r_bluetooth_design.md.
 */
#ifndef APP_BT_H
#define APP_BT_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/bluetooth.h"

typedef enum {
    BT_STATE_OFF = 0,   /* disabled, module held in reset */
    BT_STATE_RESET,     /* PD0 released, waiting for +IM_READY */
    BT_STATE_CONFIG,    /* sending the stock's config sequence */
    BT_STATE_IDLE,      /* ready, no command in flight */
    BT_STATE_SCAN,      /* scanning for an earpiece */
    BT_STATE_CONNECT,   /* connecting */
    BT_STATE_CONNECTED  /* audio link up */
} bt_state_t;

/* Target only: bring up the transport and register the event callback. */
void bt_init(void);
/* Target only: drain USART3.  Call on every pass of the main loop -- the
 * module's boot banner is ~100 bytes in ~9 ms and would overrun a 10 ms poll. */
void bt_poll(void);

/* Pure, host-testable.  Call on the 10 ms slice: it is the state machine's
 * clock (reset retry, per-command timeout). */
void bt_service_tick(void);
void bt_service_event(bt_event_t ev, const char *payload, unsigned len);

void       bt_set_enabled(bool on);
bool       bt_enabled(void);
void       bt_set_mode(uint8_t mode);        /* 0 = emitter, 1 = receiver */
void       bt_set_name(const char *name);    /* NULL/"" = unset */

/* Phase 1 item actions.  Scan is transient; the gains send the stock's own
 * value strings (see the tables in driver/bluetooth.h). */
void       bt_set_scan(bool on);
void       bt_set_spk_gain(uint8_t level);
void       bt_set_mic_gain(uint8_t level);
unsigned   bt_spk_gain_levels(void);
unsigned   bt_mic_gain_levels(void);

/* Pairing: scan (`AT+BT_SCAN=ON`, the stock's Pairing item) and the devices it
 * reports as `+IM_EARDEV`. */
void        bt_start_connect(void);
void        bt_stop_connect(void);
bool        bt_connected(void);
unsigned    bt_found_count(void);
const char *bt_found_dev(unsigned i);   /* the device's address/name text */
void        bt_connect_dev(unsigned i); /* AT+EAR_CONN=<device> */

/* The earpiece's PTT button (`+IM_EAR_PTT_KEYDOWN/UP`); tx.c keys the
 * transmitter while it is held. */
bool        bt_ptt_down(void);
bt_state_t bt_state(void);
const char *bt_version(void);
const char *bt_local_addr(void);

#endif /* APP_BT_H */
