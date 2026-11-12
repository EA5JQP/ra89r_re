/* Host test for App/k1_view.c: the K1 virtual EEPROM CHIRP addresses, over the
 * real settings layer and the host flash double (tools/host/host_hw.c).
 *
 * Checks that a download shows the port's state, that an upload lands where
 * the port keeps each piece and survives a reboot, that the stock codeplug and
 * the K1 calibration never change, and the RKT2 -> RKT3 channel move. */
#include <stdio.h>
#include <string.h>

#include "driver/py25q16.h"
#include "driver/spi_flash.h"
#include "k1_view.h"
#include "misc.h"
#include "settings.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VIEW_SIZE 0xB190u          /* the driver's MEM_SIZE */
/* CHIRP reads MEM_SIZE in 0x80 blocks, so its last block ends at 0xB200. */
static uint8_t view[0xB200u];
static uint8_t view2[0xB200u];
static uint8_t cp_before[0x4000];
static uint8_t cal_before[K1VIEW_CAL_SIZE];
static uint8_t cp_after[0x4000];
static uint8_t cal_after[K1VIEW_CAL_SIZE];

static void download(uint8_t *dst)
{
    uint32_t a;

    k1_view_open();
    for (a = 0; a < VIEW_SIZE; a += 0x80u)
        k1_view_read((uint16_t)a, &dst[a], 0x80u);
}

/* CHIRP's do_upload(): 0x0000..0xA177 in 0x80 blocks (the last one 0x78),
 * then -- only with "upload calibration" -- 0xB000..0xB18F. */
static void upload(const uint8_t *src, bool calibration)
{
    uint32_t a;

    k1_view_open();
    for (a = 0; a < 0xA178u; a += 0x80u) {
        const uint32_t n = (0xA178u - a) < 0x80u ? (0xA178u - a) : 0x80u;
        k1_view_write((uint16_t)a, &src[a], (uint8_t)n);
    }
    if (calibration)
        for (a = 0xB000u; a < VIEW_SIZE; a += 0x80u) {
            const uint32_t n = (VIEW_SIZE - a) < 0x80u ? (VIEW_SIZE - a) : 0x80u;
            k1_view_write((uint16_t)a, &src[a], (uint8_t)n);
        }
}

static void reboot(void)
{
    SettingsDefaults();
    SETTINGS_InitEEPROM();
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    uint8_t  set[K1VIEW_SETTINGS_SIZE], set2[K1VIEW_SETTINGS_SIZE];
    uint8_t  raw[16];
    unsigned i;

    reboot();
    spi_flash_read(0x0000u, cp_before, sizeof cp_before);
    spi_flash_read(K1VIEW_CAL_PHYS, cal_before, sizeof cal_before);

    /* ---- download ------------------------------------------------------- */
    download(view);
    spi_flash_read(K1_IMAGE_CH_BASE, raw, sizeof raw);
    CHECK(memcmp(&view[0], raw, 16) == 0, "channel 0 is the K1 image record");
    CHECK(rd32(&view[0]) == 14497500u, "channel 0 RX %u", (unsigned)rd32(&view[0]));
    CHECK(rd32(&view[2 * 16]) == 43037500u, "channel 2 RX");
    CHECK(memcmp(&view[0x4000], "CH-01", 5) == 0, "channel 0 name");
    CHECK(view[0xA001] == gEeprom.SQUELCH_LEVEL, "squelch byte");
    CHECK(view[0x9000 + 5 * 32] != 0xFF, "VFO record for band 6");
    CHECK(view[0x8869] == 0xFF && view[0x9FFF] == 0xFF, "hole reads 0xFF");
    CHECK(memcmp(&view[0xA170 + 3], "MIX", 3) == 0, "scanmix signature");

    /* ---- pack/unpack is a fixed point ------------------------------------ */
    k1_settings_pack(set);
    k1_settings_unpack(set);
    k1_settings_pack(set2);
    CHECK(memcmp(set, set2, sizeof set) == 0, "pack(unpack(pack)) == pack");
    for (i = 0; i < sizeof set; i++)
        if (set[i] != set2[i]) {
            printf("  differs at 0x%04X: %02X -> %02X\n", 0xA000u + i, set[i], set2[i]);
            break;
        }

    /* ---- an edited upload ----------------------------------------------- */
    memcpy(view2, view, sizeof view);
    /* channel 300: 446.00625 MHz FM, a name, attributes band 5 */
    memset(&view2[300 * 16], 0, 16);
    view2[300 * 16 + 0] = (uint8_t)44600625u;
    view2[300 * 16 + 1] = (uint8_t)(44600625u >> 8);
    view2[300 * 16 + 2] = (uint8_t)(44600625u >> 16);
    view2[300 * 16 + 3] = (uint8_t)(44600625u >> 24);
    view2[300 * 16 + 14] = 0;
    memcpy(&view2[0x4000 + 300 * 16], "PMR1            ", 16);
    view2[0x8000 + 300 * 2] = 0xE5;     /* as CHIRP leaves an erased slot: exclude set */
    view2[0x8000 + 300 * 2 + 1] = 0x00;
    /* list 1 name */
    memcpy(&view2[0x880E], "HAM1", 4);
    /* channel 1023 at the top of the record area */
    memset(&view2[1023 * 16], 0, 16);
    view2[1023 * 16 + 0] = 0x40; view2[1023 * 16 + 1] = 0x42; view2[1023 * 16 + 2] = 0xA7;
    view2[1023 * 16 + 3] = 0x02;    /* 0x02A74240 = 44515904 */
    /* VFO band 6 / VFO A: 433.500 MHz */
    {
        const uint32_t f = 43350000u;
        uint8_t *v = &view2[0x9000 + 5 * 32];
        v[0] = (uint8_t)f; v[1] = (uint8_t)(f >> 8); v[2] = (uint8_t)(f >> 16); v[3] = (uint8_t)(f >> 24);
    }
    view2[0xA001] = 7;                                   /* squelch */
    view2[0xA0A8 + 5] = 20;                              /* scan resume */
    view2[0xA15F] = (uint8_t)((3u << 4) | (view2[0xA15F] & 0x0Fu));   /* SetPwr 3 */
    view2[0xA157] = (uint8_t)((view2[0xA157] & ~(3u << 2)) | (1u << 2)); /* battery text 1 */
    memcpy(&view2[0xA0C8], "HELLO RA89R     ", 16);   /* boot message line 1 */
    /* calibration and a hole: must be dropped */
    memset(&view2[0xB000], 0x11, K1VIEW_CAL_SIZE);
    memset(&view2[0x8900], 0x22, 0x80);

    upload(view2, true);
    CHECK(k1_view_stats()->settings_dirty && k1_view_stats()->vfo_dirty, "shadows dirty");
    CHECK(k1_view_stats()->dropped >= K1VIEW_CAL_SIZE, "calibration dropped (%u)",
          (unsigned)k1_view_stats()->dropped);
    CHECK(k1_view_commit(), "commit saves");

    spi_flash_read(K1_IMAGE_CH_BASE + 300u * 16u, raw, sizeof raw);
    CHECK(rd32(raw) == 44600625u, "channel 300 in the image");
    spi_flash_read(0xA000u, raw, sizeof raw);
    CHECK(raw[0] == 0xFF, "channel 300 did not land in the K1 settings sector");
    spi_flash_read(0xA0C8u, raw, sizeof raw);
    CHECK(memcmp(raw, "HELLO RA89R", 11) == 0, "boot message at its physical address");

    /* ---- reboot: the K1 runtime reads it all back ------------------------ */
    reboot();
    CHECK(SETTINGS_FetchChannelFrequency(300) == 44600625u, "channel 300 after reboot");
    CHECK(SETTINGS_FetchChannelFrequency(1023) == 44515904u, "channel 1023 after reboot (%u)",
          (unsigned)SETTINGS_FetchChannelFrequency(1023));
    {
        char name[17];
        SETTINGS_FetchChannelName(name, 300);
        CHECK(strcmp(name, "PMR1") == 0, "name '%s'", name);
    }
    CHECK(memcmp(gListName[0], "HAM1", 4) == 0, "list name");
    CHECK(gEeprom.SQUELCH_LEVEL == 7, "squelch %u", gEeprom.SQUELCH_LEVEL);
    CHECK(gEeprom.SCAN_RESUME_MODE == 20, "scan resume %u", gEeprom.SCAN_RESUME_MODE);
    CHECK(gSetting_set_pwr == 3, "SetPwr %u (blob v6 globals)", gSetting_set_pwr);
    CHECK(gSetting_battery_text == 1, "battery text %u", gSetting_battery_text);
    {
        ChannelScanDisplayInfo_t info;
        CHECK(SETTINGS_FetchChannelScanDisplayInfo(0, FREQ_CHANNEL_FIRST + 5, &info) &&
              info.rx.Frequency == 43350000u, "VFO band 6 A after reboot");
    }

    /* ---- a second download equals what was uploaded ---------------------- */
    download(view);
    CHECK(view[0x8000 + 300 * 2] == 0x65, "exclude cleared on write (%02X)", view[0x8000 + 300 * 2]);
    view2[0x8000 + 300 * 2] = 0x65;
    CHECK(memcmp(view, view2, 0x886E) == 0, "channels/names/attrs round-trip");
    for (i = 0; i < 0x886Eu; i++)
        if (view[i] != view2[i]) {
            printf("  first difference at 0x%04X: uploaded %02X, reads %02X\n", i, view2[i], view[i]);
            break;
        }
    CHECK(memcmp(&view[0x9000], &view2[0x9000], 0xE0) == 0, "VFOs round-trip");
    CHECK(view[0xA001] == 7 && view[0xA15F] >> 4 == 3, "settings round-trip");
    for (i = 0xA000u; i < 0xA178u; i++)
        if (view[i] != view2[i] && !(i >= 0xA0B9u && i <= 0xA0BFu)) {
            printf("  note: settings byte 0x%04X uploaded %02X, reads %02X\n", i, view2[i], view[i]);
        }

    /* ---- what must never change ------------------------------------------ */
    spi_flash_read(0x0000u, cp_after, sizeof cp_after);
    spi_flash_read(K1VIEW_CAL_PHYS, cal_after, sizeof cal_after);
    CHECK(memcmp(cp_before, cp_after, sizeof cp_before) == 0, "stock codeplug 0x0000-0x3FFF untouched");
    CHECK(memcmp(cal_before, cal_after, sizeof cal_before) == 0, "K1 calibration untouched");

    /* ---- RKT2 -> RKT3: records at 0x9000 move to K1_IMAGE_CH_BASE --------- */
    {
        static const uint8_t rkt2[4] = { 'R', 'K', 'T', '2' };
        uint8_t rec[16];
        uint32_t marker;

        memset(rec, 0, sizeof rec);
        rec[0] = 0x10; rec[1] = 0x20; rec[2] = 0x30; rec[3] = 0x01;
        spi_flash_sector_erase(K1_IMAGE_CH_BASE_OLD);
        spi_flash_program(K1_IMAGE_CH_BASE_OLD + 7u * 16u, rec, sizeof rec);
        PY25Q16_WriteBuffer(0x100B8u, rkt2, sizeof rkt2, false);
        storage_import_k1();
        spi_flash_read(K1_IMAGE_CH_BASE + 7u * 16u, raw, sizeof raw);
        CHECK(memcmp(raw, rec, sizeof rec) == 0, "record 7 moved");
        spi_flash_read(K1_IMAGE_CH_BASE + 300u * 16u, raw, sizeof raw);
        CHECK(raw[0] == 0xFF, "the old (empty) slot 300 replaced the new one");
        spi_flash_read(0x100B8u, (uint8_t *)&marker, sizeof marker);
        CHECK(marker == 0x33544B52u, "marker RKT3");
        spi_flash_read(K1VIEW_CAL_PHYS + 0xC0u, raw, sizeof raw);
        CHECK(memcmp(raw, &cal_before[0xC0], sizeof raw) == 0, "calibration kept by the move");
    }

    printf("%s: %d failures\n", failures ? "FAIL" : "all k1_view checks passed", failures);
    return failures ? 1 : 0;
}
