/* The port's application state facade (see port_state.c and docs/ra89r_port.md). */
#ifndef APP_PORT_STATE_H
#define APP_PORT_STATE_H

void port_state_init(void);

/* Re-establish the VFO pointers inside gEeprom after it was replaced. */
void port_state_fixup_vfo(void);

#endif /* APP_PORT_STATE_H */
