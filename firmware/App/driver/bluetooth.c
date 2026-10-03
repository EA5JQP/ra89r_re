/* Jieli Bluetooth module driver -- see bluetooth.h and docs/ra89r_bluetooth.md.
 *
 * The command table and the response parser are a transcription of the stock's
 * own constants and dispatch (`FUN_0802213c`, `FUN_08022770`, `FUN_08022974`),
 * so they are byte-for-byte what the stock sends and accepts.  The USART3 half
 * is a polled stand-in for the stock's DMA path; it is the part that has to be
 * checked on the radio.
 */
#include "driver/bluetooth.h"

#include <string.h>

/* --------------------------------------------------------------- command set
 *
 * The exact constants from the image.  The five parameterised commands are
 * built by bt_build_param(), so they are NULL here.  The trailing "\r\n" is
 * part of each stock string and is sent as-is. */
static const char *const bt_cmd_table[BT_CMD_COUNT] = {
    [BT_CMD_GMR]              = "AT+GMR?\r\n",
    [BT_CMD_BAUD_1]           = "AT+BAUD=1\r\n",
    [BT_CMD_BAUD_2]           = "AT+BAUD=2\r\n",
    [BT_CMD_SLEEP_ON]         = "AT+SLEEP=ON\r\n",
    [BT_CMD_SLEEP_OFF]        = "AT+SLEEP=OFF\r\n",
    [BT_CMD_POWEROFF]         = "AT+POWEROFF\r\n",
    [BT_CMD_RST]              = "AT+RST\r\n",
    [BT_CMD_CONN_STATE_Q]     = "AT+CONN_STATE?\r\n",
    [BT_CMD_SPKGAIN_Q]        = "AT+SPKGAIN?\r\n",
    [BT_CMD_MICGAIN_Q]        = "AT+MICGAIN?\r\n",
    [BT_CMD_MICGAIN]          = 0,   /* AT+MICGAIN=<v>    */
    [BT_CMD_SPKGAIN]          = 0,   /* AT+SPKGAIN=<v>    */
    [BT_CMD_WRITE_NAME]       = 0,   /* AT+WRITE_NAME=<v> */
    [BT_CMD_BLE_LOCAL_Q]      = "AT+BLE_LOCAL?\r\n",
    [BT_CMD_LAST_RING_Q]      = "AT+LAST_RING?\r\n",
    [BT_CMD_BLE_SLAVE_ON]     = "AT+BLE_SLAVE=ON\r\n",
    [BT_CMD_BLE_SLAVE_OFF]    = "AT+BLE_SLAVE=OFF\r\n",
    [BT_CMD_BLE_MASTER_ON]    = "AT+BLE_MASTER=ON\r\n",
    [BT_CMD_BLE_MASTER_OFF]   = "AT+BLE_MASTER=OFF\r\n",
    [BT_CMD_BLE_SCANATCN_ON]  = "AT+BLE_SCANATCN=ON\r\n",
    [BT_CMD_BLE_SCANATCN_OFF] = "AT+BLE_SCANATCN=OFF\r\n",
    [BT_CMD_BLE_SCAN_ON]      = "AT+BLE_SCAN=ON\r\n",
    [BT_CMD_BLE_SCAN_OFF]     = "AT+BLE_SCAN=OFF\r\n",
    [BT_CMD_BLE_DISCN]        = "AT+BLE_DISCN\r\n",
    [BT_CMD_BLE_CONN_LAST]    = "AT+BLE_CONN_LAST\r\n",
    [BT_CMD_RING_CONN]        = 0,   /* AT+RING_CONN=<v>  */
    [BT_CMD_BT_LOCAL_Q]       = "AT+BT_LOCAL?\r\n",
    [BT_CMD_LAST_EAR_Q]       = "AT+LAST_EAR?\r\n",
    [BT_CMD_BT_EMITTER]       = "AT+BT=EMITTER\r\n",
    [BT_CMD_BT_RECEIVER]      = "AT+BT=RECEIVER\r\n",
    [BT_CMD_BT_SCAN_ON]       = "AT+BT_SCAN=ON\r\n",
    [BT_CMD_BT_SCAN_OFF]      = "AT+BT_SCAN=OFF\r\n",
    [BT_CMD_BT_SCANATCN_ON]   = "AT+BT_SCANATCN=ON\r\n",
    [BT_CMD_BT_SCANATCN_OFF]  = "AT+BT_SCANATCN=OFF\r\n",
    [BT_CMD_BT_DISCN]         = "AT+BT_DISCN\r\n",
    [BT_CMD_BT_CONN_LAST]     = "AT+BT_CONN_LAST\r\n",
    [BT_CMD_EAR_CONN]         = 0,   /* AT+EAR_CONN=<v>   */
    [BT_CMD_BT_PAIRCLR]       = "AT+BT_PAIRCLR\r\n",
    [BT_CMD_BT_CALL_ON]       = "AT+BT_CALL=ON\r\n",
    [BT_CMD_BT_CALL_OFF]      = "AT+BT_CALL=OFF\r\n",
};

const char *bt_cmd_string(bt_cmd_t cmd)
{
    if ((unsigned)cmd >= BT_CMD_COUNT)
        return 0;
    return bt_cmd_table[cmd];
}

unsigned bt_build_param(bt_cmd_t cmd, const char *value, char *buf, unsigned cap)
{
    static const char *const prefix[] = {
        [BT_CMD_MICGAIN]    = "AT+MICGAIN=",
        [BT_CMD_SPKGAIN]    = "AT+SPKGAIN=",
        [BT_CMD_WRITE_NAME] = "AT+WRITE_NAME=",
        [BT_CMD_RING_CONN]  = "AT+RING_CONN=",
        [BT_CMD_EAR_CONN]   = "AT+EAR_CONN=",
    };
    const char *p;
    unsigned n, v;

    if ((unsigned)cmd >= BT_CMD_COUNT || !buf || !value)
        return 0;
    p = prefix[cmd];
    if (!p)
        return 0;

    n = (unsigned)strlen(p);
    v = (unsigned)strlen(value);
    /* prefix + value + "\r\n" + NUL */
    if (n + v + 3u > cap)
        return 0;

    memcpy(buf, p, n);
    memcpy(buf + n, value, v);
    buf[n + v] = '\r';
    buf[n + v + 1] = '\n';
    buf[n + v + 2] = '\0';
    return n + v + 2u;
}

/* ------------------------------------------------------------ response parser
 *
 * The prefixes and their order are the stock's (`FUN_08022974`).  A line is
 * matched against the first prefix it starts with; the stock tests them in
 * exactly this order, so a line that could match two takes the earlier one.
 * The payload pointer is set for the events that carry data. */
struct bt_event_spec {
    const char *prefix;
    bt_event_t  event;
    int         has_payload;
};

static const struct bt_event_spec bt_events[] = {
    { "RDTP",               BT_EV_BINARY_FRAME,     0 },
    { "+IM_READY",          BT_EV_READY,            0 },
    { "+OK",                BT_EV_OK,               0 },
    { "+ERROR",             BT_EV_ERROR,            0 },
    { "+IM_VERSION:",       BT_EV_VERSION,          1 },
    { "+IM_BT_EMITTER",     BT_EV_BT_EMITTER,       0 },
    { "+IM_NAME_EQUALLY",   BT_EV_NAME_EQUALLY,     0 },
    { "+IM_BT_RECEIVER",    BT_EV_BT_RECEIVER,      0 },
    { "+IM_BLE_MASTER",     BT_EV_BLE_MASTER,       0 },
    { "+IM_BLE_SLAVE",      BT_EV_BLE_SLAVE,        0 },
    { "+IM_CONN_STATE:",    BT_EV_CONN_STATE,       1 },
    { "+IM_SCO_CONN",       BT_EV_SCO_CONN,         0 },
    { "+IM_CALL_CONED",     BT_EV_CALL_CONNECTED,   0 },
    { "+IM_CALL_DISCONED",  BT_EV_CALL_DISCONNECTED,0 },
    { "+IM_SCO_DISCN",      BT_EV_SCO_DISCONNECT,   0 },
    { "+IM_EARDEV:",        BT_EV_EARDEV,           1 },
    { "+IM_BT_SCAN_STOP",   BT_EV_BT_SCAN_STOP,     0 },
    { "+IM_BT_EAR_CONN",    BT_EV_BT_EAR_CONN,      0 },
    { "+IM_BT_DISCN",       BT_EV_BT_DISCONNECT,    0 },
    { "+IM_EAR_PTT_KEYDOWN",BT_EV_EAR_PTT_DOWN,     0 },
    { "+IM_EAR_PTT_KEYUP",  BT_EV_EAR_PTT_UP,       0 },
    { "+IM_BLE_LOCAL",      BT_EV_BLE_LOCAL,        1 },
    { "+IM_BT_LOCAL",       BT_EV_BT_LOCAL,         1 },
};

bt_event_t bt_parse_line(const char *line, unsigned len,
                         const char **payload, unsigned *payload_len)
{
    unsigned i;

    if (payload)
        *payload = 0;
    if (payload_len)
        *payload_len = 0;
    if (!line || len == 0)
        return BT_EV_NONE;

    /* The stock strips one leading '+' from a "++" line (the module's escape
     * for a literal '+'). */
    if (len >= 2u && line[0] == '+' && line[1] == '+') {
        line++;
        len--;
    }

    for (i = 0; i < sizeof bt_events / sizeof bt_events[0]; i++) {
        unsigned plen = (unsigned)strlen(bt_events[i].prefix);

        if (len < plen)
            continue;
        if (memcmp(line, bt_events[i].prefix, plen) != 0)
            continue;
        if (bt_events[i].has_payload) {
            if (payload)
                *payload = line + plen;
            if (payload_len)
                *payload_len = len - plen;
        }
        return bt_events[i].event;
    }
    return BT_EV_NONE;
}

/* ------------------------------------------------------------- transmit path
 *
 * bluetooth_hw_write() is the one hardware dependency.  On the target it is
 * defined at the bottom of this file; a host test compiles with
 * -DBLUETOOTH_HOST_TEST and provides its own recorder. */
void bluetooth_send(const char *text)
{
    if (text)
        bluetooth_hw_write((const uint8_t *)text, (unsigned)strlen(text));
}

void bluetooth_send_cmd(bt_cmd_t cmd)
{
    const char *s = bt_cmd_string(cmd);

    if (s)
        bluetooth_send(s);
}

void bluetooth_send_param(bt_cmd_t cmd, const char *value)
{
    char buf[80];

    if (bt_build_param(cmd, value, buf, sizeof buf) != 0u)
        bluetooth_send(buf);
}

/* ---------------------------------------------------------------- receive path
 *
 * Accumulate bytes into a line, split on '\n', strip a trailing '\r' and hand
 * the line to the parser.  This is what `FUN_08022900` does before it calls
 * `FUN_08022974`.  No hardware, so it is host-testable. */
static void (*s_event_cb)(bt_event_t, const char *, unsigned);
static char   s_line[128];
static unsigned s_line_len;

void bluetooth_set_event_cb(void (*cb)(bt_event_t, const char *, unsigned))
{
    s_event_cb = cb;
}

static void bt_dispatch_line(void)
{
    const char *payload;
    unsigned payload_len;
    bt_event_t ev;

    if (s_line_len == 0u)
        return;
    if (s_line[s_line_len - 1u] == '\r')
        s_line_len--;
    s_line[s_line_len] = '\0';

    ev = bt_parse_line(s_line, s_line_len, &payload, &payload_len);
    if (ev != BT_EV_NONE && s_event_cb)
        s_event_cb(ev, payload, payload_len);
}

void bluetooth_feed(const uint8_t *data, unsigned len)
{
    unsigned i;

    if (!data)
        return;
    for (i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (c == '\n') {
            bt_dispatch_line();
            s_line_len = 0;
        } else if (s_line_len < sizeof s_line - 1u) {
            s_line[s_line_len++] = (char)c;
        } else {
            s_line_len = 0;             /* overflow: drop the line, as the stock would */
        }
    }
}

/* ------------------------------------------------------------------- hardware
 *
 * USART3 on PB10 (TX) / PB11 (RX), AF2, 115200 8N1.  This is a **polled**
 * transport; the stock uses DMA1 for both directions (see the doc).  Compiled
 * out for host tests, which provide bluetooth_hw_write() themselves. */
#ifndef BLUETOOTH_HOST_TEST

#include "board.h"
#include "driver/gpio.h"

void bluetooth_hw_write(const uint8_t *data, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        while (!(BOARD_BT_UART->SR & USART_SR_TXE))
            ;
        BOARD_BT_UART->DR = (uint32_t)data[i];
    }
    while (!(BOARD_BT_UART->SR & USART_SR_TC))
        ;
}

void bluetooth_init(void)
{
    /* PB10/PB11 as USART3 AF2, pull-up on the idle-high RX line. */
    gpio_config_af(BOARD_BT_UART_PORT, BT_UART_TX_PIN | BT_UART_RX_PIN,
                   BT_UART_AF, 1u);

    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;

    /* 8N1, no flow control, no DMA -- the stock's CR1 value is 0xc (RE|TE). */
    BOARD_BT_UART->CR1 = 0;
    BOARD_BT_UART->CR2 = 0;
    BOARD_BT_UART->CR3 = 0;
    (void)BOARD_BT_UART->SR;             /* clear ORE/FE/NE/PE: SR then DR */
    (void)BOARD_BT_UART->DR;

    BOARD_BT_UART->BRR = (BOARD_APB1_HZ + (BT_UART_BAUD / 2u)) / BT_UART_BAUD;
    BOARD_BT_UART->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    while (BOARD_BT_UART->SR & USART_SR_RXNE)
        (void)BOARD_BT_UART->DR;
}

void bluetooth_poll(void)
{
    uint8_t buf[32];
    unsigned n = 0;

    while ((BOARD_BT_UART->SR & USART_SR_RXNE) && n < sizeof buf)
        buf[n++] = (uint8_t)BOARD_BT_UART->DR;
    if (n)
        bluetooth_feed(buf, n);
}

#endif /* !BLUETOOTH_HOST_TEST */
