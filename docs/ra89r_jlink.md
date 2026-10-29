# RA89R SWD / J-Link debugging

Live debugging of the radio's PY32F403 over SWD, driven by the
[`jlink-mcp`](https://github.com/Klievan/jlink-mcp) MCP server.  This is a
**second channel**: it does not replace the serial bootloader — firmware still
goes in as `.icf` over the Kenwood programming jack
([`ra89r_bootloader.md`](ra89r_bootloader.md)).  J-Link adds halt / registers /
memory / peripherals / crash decode, and (optionally) a way to flash the raw
application image directly.

The write-up answers one question up front — what an `.icf` is and what the
bootloader does with it — because that is what decides what J-Link may and may
not touch.

## 1. What an `.icf` is, and what the bootloader does with it

An `.icf` is **not** a raw image and it is **not** stored raw in the radio.  Per
[`tools/ra89r.py`](../tools/ra89r.py) (the codec is the authority) and the
bootloader handler at `0x08000BE0` ([`ra89r_bootloader.md`](ra89r_bootloader.md)
§3):

* the file is **ASCII hex, one record per CR-separated line**; a record is
  `[6-byte header][payload][1 check byte]`;
* it is **XOR-ciphertext**: every header byte is XOR'd with the family
  `BASELINE` (`0x66` for the RA89R), and the payload **and the check byte** are
  XOR'd with a per-record key `key = h[0]^h[1]^h[2]^h[3]^h[4]^h[5] ^ BASELINE`;
* the header also carries the destination:
  `length = (h[0]<<8)|h[1]` and `addr = ((h[2]<<16)|(h[3]<<8)|h[4]) * 0x100`.

So when the user says "the `.icf` is an encrypted binary and the bootloader
decrypts it into a raw binary at `0x08004000`", that is essentially right, with
two precisions worth keeping:

* **"encrypted" means XOR**, not a cipher with a secret key: the key is derived
  from the record's own header, so anyone can decrypt it.  It is obfuscation /
  encryption in the loose sense, and it is *why the old tool needed a probe* —
  the check byte is XOR'd too, so it looks like a keyed hash until that XOR is
  undone.
* **it is decrypted record by record, at each record's own address**, not as one
  blob.  The application's records simply start at `0x08004000`
  (`mkicf … --base 0x08004000`); the bootloader writes each record's plaintext
  payload to flash at that record's address, erasing and merging 2 KB pages as
  it goes.  Full-image flashes and partial-page records both work.

The bootloader **refuses** any record whose address falls in
`0x08000000-0x08003FFF` (`0x08000C30`, [`ra89r_bootloader.md`](ra89r_bootloader.md)
§2), and `tools/ra89r_flash.py` refuses the same range unless
`--allow-bootloader-region` is passed.  That 16 KB region is the bootloader
itself.

*Located* (read out of `bootloader.bin` and the codec), not a hardware claim.

## 2. The rule this note exists to enforce

**Never program `0x08000000-0x08003FFF`.**  That is the stock bootloader, the
only way back to stock; the application is linked at `0x08004000` into a 368 KB
region above it.  Both flashing paths already refuse the region:

* serial: the bootloader and `tools/ra89r_flash.py` reject those records;
* J-Link: the custom device declares its flash bank `AlwaysPresent="0"`, which
  keeps it out of J-Link's blanket `erase` command (see §3).  A normal flash
  erases only the 2 KB sectors the image actually occupies, so an app image at
  `0x08004000` leaves the bootloader untouched.

There is no recovery from a wiped bootloader except an external dump, so treat
`erase` as a no-op by design here and never point a flash below `0x08004000`.

## 3. Custom J-Link device: `PY32F403xD`

J-Link V7.98i ships **no Puya MCU support** (its compiled device list has no
`Puya` entry at all — only Puya SPI-NOR parts appear on SEGGER's site).  The part
is therefore declared with the J-Link Device Support Kit, using the vendor's own
CMSIS flash algorithm.  The device is visible to J-Link as **`PY32F403xD`**:

| fact | value | source |
|---|---|---|
| core | Cortex-M4F | `Puya.PY32F4xx_DFP.pdsc` (`DcoreVersion` r0p1, FPU, MPU) |
| flash | 384 KB @ `0x08000000` | part `PY32F403xD`, `IROM1` size `0x60000` |
| SRAM | 64 KB @ `0x20000000` | `IRAM1` size `0x10000` |
| flash page | 2048 bytes | FLM sectorization (`0x800`) |
| loader | `PY32F403xx_384.FLM` | `PY32F4xx_DFP` CMSIS/Flash, `FLASH_ALGO_TYPE_OPEN` |

Files (J-Link's central `JLinkDevices` folder, found recursively to depth 4,
[max. 4 levels per SEGGER KB](https://kb.segger.com/J-Link_Device_Support_Kit)):

```
~/.config/SEGGER/JLinkDevices/Puya/PY32F403/JLinkDevices.xml
~/.config/SEGGER/JLinkDevices/Puya/PY32F403/PY32F403xx_384.FLM
```

`JLinkDevices.xml` (the bootloader-safety attribute is the point):

```xml
<Database>
  <Device>
    <ChipInfo Vendor="Puya" Name="PY32F403xD" WorkRAMAddr="0x20000000"
              WorkRAMSize="0x10000" Core="JLINK_CORE_CORTEX_M4" />
    <FlashBankInfo Name="Internal code flash (app region, bootloader excluded)"
                   BaseAddr="0x08000000" AlwaysPresent="0">
      <LoaderInfo Name="PY32F403xD_384" MaxSize="0x60000"
                  Loader="PY32F403xx_384.FLM" LoaderType="FLASH_ALGO_TYPE_OPEN" />
    </FlashBankInfo>
  </Device>
</Database>
```

The `.FLM` and the SVD come from the `PY32F4xx_Firmware` submodule:

```
PY32F4xx_Firmware/Pack/GCC/.pack/Puya/PY32F4xx_DFP/CMSIS/Flash/PY32F403xx_384.FLM
PY32F4xx_Firmware/Pack/GCC/.pack/Puya/PY32F4xx_DFP/CMSIS/SVD/py32f403xx.svd
```

`WorkRAMAddr`/`WorkRAMSize` is SRAM the loader may use; the same area is not
reserved by the radio's own firmware while it is halted, which is the normal
case for a debug session.

## 4. Host toolchain

`jlink-mcp` is a Node program.  This host has no system Node (`sudo` is not
available), so it lives under `~/.local`:

```sh
# Node.js LTS (v24.x), user-local — no sudo
mkdir -p ~/.local/opt && tar -C ~/.local/opt -xf node-v24.22.0-linux-x64.tar.xz
for b in node npm npx corepack; do
  ln -sf ~/.local/opt/node-v24.22.0-linux-x64/bin/$b ~/.local/bin/$b
done

# build the server
cd ~/Repos/jlink-mcp
export PATH="$HOME/.local/bin:$PATH"
npm install && npm run build          # -> out/mcp/standalone.js
```

The SVD is copied out of the submodule to a stable path (the submodule is
optional at runtime):

```sh
cp PY32F4xx_Firmware/Pack/GCC/.pack/Puya/PY32F4xx_DFP/CMSIS/SVD/py32f403xx.svd \
   ~/.local/share/ra89r-msvd/py32f403xx.svd
```

### OpenCode MCP registration

Registered globally in `~/.config/opencode/opencode.json` (V2 shape,
`mcp.servers`), alongside the existing `ghidra-mcp` entry:

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "servers": {
      "jlink": {
        "type": "local",
        "command": [
          "/home/gonzalo/.local/bin/node",
          "/home/gonzalo/Repos/jlink-mcp/out/mcp/standalone.js"
        ],
        "environment": {
          "JLINK_DEVICE": "PY32F403xD",
          "JLINK_INSTALL_DIR": "/opt/SEGGER/JLink",
          "JLINK_INTERFACE": "SWD",
          "JLINK_SPEED": "4000",
          "SVD_PATH": "/home/gonzalo/.local/share/ra89r-msvd/py32f403xx.svd"
        }
      }
    }
  }
}
```

| env | value | why |
|---|---|---|
| `JLINK_DEVICE` | `PY32F403xD` | the custom device from §3 |
| `JLINK_INSTALL_DIR` | `/opt/SEGGER/JLink` | symlink to `JLink_V798i`, holds `JLinkExe` |
| `JLINK_INTERFACE` | `SWD` | 2-wire debug |
| `JLINK_SPEED` | `4000` kHz | conservative start; lower it if attach is flaky |
| `SVD_PATH` | `py32f403xx.svd` | names peripheral bits in `read_peripheral` / `decode_register` |

`JLINK_RTT_ADDR` is deliberately unset: the RA89R has no SEGGER RTT control
block (its debug channel is the 115200 USART1 console,
[`firmware.md`](firmware.md)), so the RTT tools are not applicable here.

Source-level `gdb_*` tools additionally need an ARM `arm-none-eabi-gdb`; this
host has only an x86 `gdb`, so those tools are the one part that does not work
until the toolchain is installed.  Everything else (halt/resume/reset, memory,
registers, `snapshot`, `diagnose_crash`, peripherals, serial flash) does not use
the GDB client.

## 5. SWD wiring: the pins are shared with the LEDs

`PA13`/`PA14` **are** the Cortex-M `SWDIO`/`SWCLK` pads, and the stock firmware —
and the port — reconfigure both as push-pull GPIO outputs for the status LEDs
([`ra89r_pins.md`](ra89r_pins.md), [`ra89r_rffeatures.md`](ra89r_rffeatures.md)).
Once the application runs, it drives the debug pads and a plain SWD attach fails.

Consequences for a debug session:

* the pads must be broken out on the PCB and wired to the probe
  (`SWDIO`/`SWCLK`/`GND`, plus **`nRESET`** — see the next point);
* with the app running, J-Link must attach **connect-under-reset**, which needs
  the `nRESET` line: at reset the pads are still SWD, so the probe wins the race
  before `GPIO_Init` takes them (`GPIOA` mask `0x6000` at `0x08013CCE`);
* alternatively, attach while the radio sits in the bootloader (update mode) —
  power on with **both PTTs held** ([`../tools/FLASHING.md`](../tools/FLASHING.md)
  §6) — where the application's LED configuration has not run.  Whether the
  bootloader itself leaves `PA13`/`PA14` as SWD is not measured, but the app is
  not running, so the pads should still be at their reset default;
* a J-Link "connect under reset" with no `nRESET` wired cannot win that race —
  the observed failure mode is `Failed to attach to CPU`, sometimes
  `Failed to initialize DAP`.

The probe also supplies **`VTref`** for level sensing; a reading near 3.3 V means
the target is powered.  Conversely, the probe **cannot power the radio**:
`VTref` (pin 1) is a *sense* input that must be driven by the target's own 3.3 V,
and the only supply on the 20-pin header is **5 V on pin 19** — fused around
300 mA and inactive by default on most models (this probe reports as `J-Link CE`,
HW V9.60).  Pin 19 feeds the **whole board**, not the MCU, so on a battery radio
with a 3.3 V rail shared by MCU, RF section and backlight it is the wrong
voltage, far too little current, and back-feeding a self-powered board is
unsafe.  Power the radio from its own battery/USB; wire **`GND` + `VTref` +
`SWDIO` + `SWCLK` (+ `nRESET`)** and leave pin 19 **disconnected**.  If a
debugger-driven power cycle is ever wanted (e.g. to force an attach at reset
without `nRESET`), switch the radio's own power externally, not via the probe.

### 5.1 Does this need a build option? — flashing, no; live debugging, yes

The tempting framing — "an `#ifdef` for when we flash via J-Link and another for
when we flash via `.icf`" — puts the switch on the wrong axis.  How the image
gets *in* is independent of what the running application does with the pads:

* **Flashing via `.icf` (serial):** the bootloader runs, then the app runs and
  configures `PA13`/`PA14` as LED outputs exactly as it does today.  Nothing to
  change.
* **Flashing via J-Link:** the probe resets the core and halts it at the reset
  vector, i.e. **before `led_init()` ever runs** (`driver/led.c`), so the pads
  are still in their SWD reset state and J-Link owns them for the whole
  erase/program.  This is why J-Link *flashing* needs no firmware change at all —
  it only needs **`nRESET` wired** (or the radio held in update mode, §5), so the
  probe can win the race deliberately.
* **A live SWD link against the running app:** this is the case that needs a
  firmware switch.  `led_init()` calls
  `gpio_config_output(LED_PORT, LED_RED_PIN | LED_GREEN_PIN)`, which switches
  `PA13`/`PA14` to GPIO outputs and **drops the SWD link the moment the app
  starts** — after that no breakpoint, step or `snapshot` can reach it.  For a
  debug session the application must leave those pads alone.

So the option belongs on **debug build vs normal build**, not on the flash
method.  Wired as a single compile-time flag next to the LED pins (the
`k1_features.h` / `board_pins.h` pattern the port already uses), it would:

* skip the `PA13`/`PA14` line in `led_init()`, leaving them at their SWD reset
  default, and
* make `led_drive()` / `led_set()` inert so the calls from `tx.c`, `rx.c`,
  `bk4819.c` and `main.c` cannot drive the pads.

The one real cost: the red/green status LED (the `Rx.Light` green and the TX red)
is gone in a debug build, because those two physical LEDs are wired to the two
SWD pads — SWD needs both, so you cannot keep one and drop the other, and moving
the LED to a free `GPIOC`/`GPIOD` line would mean a board rework, so it is not an
option.  A release/normal build keeps the LEDs and takes the loss of live SWD.

Consistent with the repo's workflow, that firmware change would stay on a
development branch until it is validated on the radio; the notes here describe
the mechanism, not a shipped feature.

## 6. Flashing through J-Link (optional)

This is an *alternative* to the serial bootloader, not an addition.  The `.icf`
cannot be handed to J-Link — feed it the **raw** image at its base:

| file | what to use |
|---|---|
| `firmware/build/<preset>/ra89r_fw.bin` | pass as a `.bin` with `baseAddress = 0x08004000` |
| `firmware/build/<preset>/ra89r_fw.hex` / `.elf` | addresses are carried in the file; flash directly |
| a decoded stock image | `tools/ra89r.py decode STOCK.icf work/fw.bin` → flash `work/fw.bin` at `0x08004000` |

In the MCP server: `flash { filePath: "…/ra89r_fw.bin", baseAddress: "0x08004000" }`.
Never pass `baseAddress = 0x08000000`, and never call `erase` — with
`AlwaysPresent="0"` it is a no-op by design, which is exactly the guard rail
wanted for this radio.

For everyday firmware updates keep using the serial bootloader
([`../tools/FLASHING.md`](../tools/FLASHING.md)); it is the supported, verified
path and it never touches the bootloader region.

## 7. Reproduce / quick start

```sh
export PATH="$HOME/.local/bin:$PATH"

# 1. server is up and the device + SVD resolve (no target needed for the last two)
cd ~/Repos/jlink-mcp && node out/mcp/standalone.js     # speaks MCP on stdio
#   check_setup        -> "OK target device — PY32F403xD", "OK SVD — loaded"
#   list_peripherals   -> GPIOA..GPIOE @ 0x48000000…

# 2. with SWD + nRESET wired, a session looks like:
#   start_debug_session ; snapshot ; diagnose_crash ; read_peripheral GPIOA
```

A standalone sanity check of the device definition, without a target:

```sh
JLinkExe -device PY32F403xD -if SWD -speed 4000 \
  -CommanderScript <(printf 'exit\n') 2>&1 | grep -E 'Device|XML|Loader|FlashBank'
#   "XML file found at: …/Puya/PY32F403/JLinkDevices.xml"
#   "Device "PY32F403XD" selected."
#   "Flash bank @ 0x08000000: Parsing sectorization info from ELF file"
#   "OFL: EraseSector() present", "OFL: ProgramPage() present"
```

## 8. Status

* **Verified on the host (no target):** the J-Link device definition is parsed
  and accepted (`Device "PY32F403XD" selected`, FLM sectorization read); the MCP
  server starts, lists its tools, reports `PY32F403xD`, loads the SVD, and
  answers `list_peripherals` from it.
* **Located, not verified on the radio:** every SWD claim above, and J-Link
  flashing.  At the time of writing the CPU is not wired to the probe, so no
  attach, no flash and no live register/peripheral read has been done on the
  RA89R.  The `AlwaysPresent="0"` protection is per the SEGGER DSK semantics,
  not yet observed on hardware.
* **Unchanged:** the serial bootloader flashing path.

## 9. Open points

1. **SWD attach recipe on the radio.**  Does connect-under-reset with `nRESET`
   wired work with the stock app running, and does update mode need the reset
   line at all?  Not yet tried.
2. **`0x08000000-0x08003FFF` protection under J-Link.**  Confirm on hardware
   that a `BASE=0x08004000` flash leaves the 16 KB bootloader byte-identical,
   and that `erase` is indeed a no-op.
3. **`nRESET` routing.**  Which pad/test point carries `nRST` on the RA89R PCB is
   not recorded in this repository.
4. **RTT.**  Not available (no `_SEGGER_RTT` in the firmware); the console is the
   logging channel.  If RTT is ever added to the port, set `JLINK_RTT_ADDR` to
   `_SEGGER_RTT` so collection survives a reset.

## Related

* [`ra89r_bootloader.md`](ra89r_bootloader.md) — the serial protocol, the `E2`
  record validator, and the page erase/merge the bootloader performs.
* [`../tools/ra89r.py`](../tools/ra89r.py) — the `.icf` codec, the authority for
  the record layout and the XOR.
* [`ra89r_pins.md`](ra89r_pins.md) — `PA13`/`PA14` are the SWD pads.
* [`../tools/FLASHING.md`](../tools/FLASHING.md) — the supported flashing path.
* J-Link Device Support Kit — <https://kb.segger.com/J-Link_Device_Support_Kit>.
