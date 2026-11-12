/* The radio's CHIRP programming side on a PC: App/k5prog.c + App/k1_view.c
 * over the real settings layer and the host flash double, served on a PTY so
 * the unmodified CHIRP driver (tools/webchirp/chirp_e2e.py) can talk to it.
 *
 *   k5prog_sim <file-to-write-the-pty-path-into>
 *
 * A 0x05DD reset commits and "reboots" (SettingsDefaults + SETTINGS_InitEEPROM)
 * and keeps serving, with the flash kept: the next session sees what the last
 * one wrote, as on the radio.  On stdin EOF / SIGTERM it prints a checksum of
 * the regions that must never change (stock codeplug, K1 calibration) next to
 * the one taken at start, and exits non-zero if they differ.
 *
 * Built by tools/webchirp/chirp_e2e.py (see the command at its top). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "driver/spi_flash.h"
#include "k1_view.h"
#include "k5prog.h"
#include "settings.h"

static int               s_fd = -1;
static volatile sig_atomic_t s_stop;
static unsigned          s_resets;

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void sim_send(const uint8_t *b, uint16_t n)
{
    while (n > 0u) {
        const ssize_t w = write(s_fd, b, n);

        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return;
        }
        b += w;
        n = (uint16_t)(n - w);
    }
}

static void sim_hello(void)
{
    k1_view_open();
}

static void sim_reset(void)
{
    const k1_view_stats_t *v = k1_view_stats();

    printf("[sim] reset: %u blocks changed flash, %u unchanged, %u bytes dropped, settings %s, vfo %s\n",
           (unsigned)v->written, (unsigned)v->unchanged, (unsigned)v->dropped,
           v->settings_dirty ? "written" : "unchanged", v->vfo_dirty ? "written" : "unchanged");
    (void)k1_view_commit();
    SettingsDefaults();
    SETTINGS_InitEEPROM();
    s_resets++;
    printf("[sim] rebooted (%u); squelch %u, scan resume %u\n", s_resets,
           (unsigned)gEeprom.SQUELCH_LEVEL, (unsigned)gEeprom.SCAN_RESUME_MODE);
    fflush(stdout);
}

static const k5prog_ops_t ops = {
    sim_send, k1_view_read, k1_view_write, sim_hello, sim_reset, "RA89R F4HWN sim"
};

static uint32_t region_sum(uint32_t addr, uint32_t len)
{
    uint8_t  buf[256];
    uint32_t sum = 2166136261u, i, done = 0;

    while (done < len) {
        const uint32_t k = (len - done) < sizeof buf ? (len - done) : sizeof buf;

        spi_flash_read(addr + done, buf, k);
        for (i = 0; i < k; i++)
            sum = (sum ^ buf[i]) * 16777619u;
        done += k;
    }
    return sum;
}

static void on_signal(int sig)
{
    (void)sig;
    s_stop = 1;
}

int main(int argc, char **argv)
{
    uint32_t cp0, cal0;
    int      slave;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <pty-path-file>\n", argv[0]);
        return 2;
    }

    SettingsDefaults();
    SETTINGS_InitEEPROM();
    cp0  = region_sum(0x0000u, 0x4000u);
    cal0 = region_sum(K1VIEW_CAL_PHYS, K1VIEW_CAL_SIZE);

    s_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (s_fd < 0 || grantpt(s_fd) != 0 || unlockpt(s_fd) != 0) {
        perror("pty");
        return 1;
    }
    {
        struct termios t;
        /* Keep the slave open ourselves so a client closing it is not an EOF. */
        slave = open(ptsname(s_fd), O_RDWR | O_NOCTTY);
        tcgetattr(slave, &t);
        cfmakeraw(&t);
        tcsetattr(slave, TCSANOW, &t);
    }
    {
        FILE *f = fopen(argv[1], "w");
        if (f == 0) {
            perror(argv[1]);
            return 1;
        }
        fprintf(f, "%s\n", ptsname(s_fd));
        fclose(f);
    }
    printf("[sim] serving on %s\n", ptsname(s_fd));
    fflush(stdout);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    k5prog_init(&ops);

    while (!s_stop) {
        struct pollfd p[2] = { { s_fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        uint8_t       buf[512];

        if (poll(p, 2, 200) < 0)
            continue;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            char tmp[64];
            if (read(0, tmp, sizeof tmp) <= 0)
                break;
        }
        if (p[0].revents & POLLIN) {
            const ssize_t n = read(s_fd, buf, sizeof buf);
            ssize_t i;

            for (i = 0; i < n; i++)
                (void)k5prog_feed(buf[i], now_ms());
        }
    }

    {
        const uint32_t cp1  = region_sum(0x0000u, 0x4000u);
        const uint32_t cal1 = region_sum(K1VIEW_CAL_PHYS, K1VIEW_CAL_SIZE);
        const k5prog_stats_t *st = k5prog_stats();

        printf("[sim] frames %u (bad %u, stale %u), reads %u, writes %u, resets %u\n",
               (unsigned)st->frames, (unsigned)st->bad, (unsigned)st->stale,
               (unsigned)st->reads, (unsigned)st->writes, s_resets);
        printf("[sim] stock codeplug %s, K1 calibration %s\n",
               cp0 == cp1 ? "unchanged" : "CHANGED", cal0 == cal1 ? "unchanged" : "CHANGED");
        close(slave);
        return (cp0 == cp1 && cal0 == cal1) ? 0 : 1;
    }
}
