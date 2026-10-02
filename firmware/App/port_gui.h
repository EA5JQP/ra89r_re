/* The port's screen and key loop (see port_gui.c). */
#ifndef APP_PORT_GUI_H
#define APP_PORT_GUI_H

#include "ui/ui.h"

void port_gui_init(void);
void port_gui_poll(void);

/* Periodic repaint (status line, signal read-out). */
void port_gui_tick(uint32_t now_ms);
void port_gui_screen(GUI_DisplayType_t screen);
void port_gui_welcome(void);

#endif /* APP_PORT_GUI_H */
