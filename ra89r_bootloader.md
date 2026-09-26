# RA89R bootloader / flashing protocol

How the stock bootloader accepts a firmware image over the radio's programming
port, reverse engineered from two sources:

* `bootloader.bin` — 16 KB, mapped at `0x08000000`, dumped from the radio over
  the serial port.  All "bootloader `0x08…`" addresses below are offsets into
  this image.
* The CPS firmware updater `Radio_UpData_All.exe` (.NET, shipped inside the
  installer, decompiled with `ilspycmd`).  It is the program this protocol was
  designed against, so its behaviour is the reference for the host side.

Tooling built from this: `tools/ra89r_flash.py` (host side) and
`ra89r.py mkicf` (wrap a raw image into records the bootloader accepts).

## 1. Port

The bootloader talks on **USART1, PB6 (TX) / PB7 (RX), AF2** — the same port the
stock application uses; the updater opens it at **9600 8N1**
(`EnterWriteModPro`: `serialPort1.BaudRate = 9600`).  It is the radio's
programming/serial jack (the Kenwood-style connector on the RA89R).

**Use the Kenwood connector, not USB-C.**  The MCU does have a USB device
peripheral (0x40005C00; the vendor SDK even ships CherryUSB, and RCC has
`USBEN`/`USBPRE`/HSI48), so the USB-C port is most likely wired to the MCU's
USB pins — but **neither the bootloader nor the stock application ever enables
or touches it**: no write to `RCC_APB1ENR` bit 23 (`USBEN`), zero references to
0x40005C00 in either image (verified over the full disassembly of both), and
the CPS updater uses `System.IO.Ports.SerialPort` only (COM ports).  So USB-C
can neither flash this radio nor talk to the stock firmware; the bootloader's
only port is USART1.''')
open(p,'w').write(s)

p='firmware/README.md'; s=open(p).read()
s=s.replace(## Which connector

**Use the Kenwood-style programming jack, not USB-C.**  The bootloader speaks
only USART1 (PB6/PB7), which is the Kenwood jack; the USB-C port goes to the
MCU's USB pins (PY32F403 has a USB device peripheral at 0x40005C00) but nothing
in the stock bootloader or application ever enables it, and the CPS updater only
knows COM ports.  See `../ra89r_bootloader.md` §1.

(That USB port is still interesting *later*: the vendor SDK ships CherryUSB, and
the UV-K1/K5V3 port tree already implements a USB CDC console — so USB-C could
become a log/console port for this firmware once USB + the 48 MHz clock are set
up.  Flashing would still be Kenwood-only.)

## Flashing — via the stock bootloader

## 2. Frames

```
FE FE EE EF <cmd> <payload...> FD          ("FE FE EE EF" = bootloader 0x08003455)
```

**Program frames are length driven, not `FD` delimited.**  The stock records
contain **726 `0xFD` bytes inside their payloads** (340 counting the header and
check byte as well), so no parser could end a frame on the first `0xFD`.  For
`E2` the bootloader reads the 6-byte record header, derives the payload length
from it (the same `(h[0]<<8)|h[1]` rule), and consumes exactly that many bytes
plus the check byte; the trailing `0xFD` is written by the host (as the updater
does) and is simply skipped.  The fixed-length commands (`E0` is checked to be
exactly 17 bytes, 0x0800310A) carry short ASCII payloads, where an `FD`
terminator is meaningful.

Consequence worth knowing: a corrupted *header* desyncs the link (the receiver
waits for the wrong number of bytes), so a `FAIL` or a missing ack should be
recovered by re-sending the `E0` handshake before retrying — `tools/ra89r_flash.py`
does that automatically.

The bootloader dispatches on `<cmd>` (0x08003098):

| cmd | payload | handler | meaning |
|---|---|---|---|
| `E0` | 11 bytes `UP8600BYARR` | 0x08003104 | handshake / "who are you" — answers with the identity announcement (section 4). The frame must be exactly 17 bytes (`cmp len,#0x11`) |
| `E1` | `CLEAR` (5 bytes) | 0x080005DC | erase command (defined by the updater as `ErasureCom`; its own flow never sends it) |
| `E2` | one raw `.icf` record | 0x08000BE0 | program — validate and write, then answer PASS/FAIL (section 3) |
| `E3` | `READ05` | 0x08002740 | read (updater constant `ReadProCom = E3 "READ05" FD`; the handler in this build only answers) |
| `E4` | `EXIT` | 0x08000B04 | leave the bootloader (updater sends `FE FE EE EF E4 45 58 49 54 FD`) |
| `E5` | `BAUDRATE` + two ASCII digits | 0x080004AC | change baud (section 5) |

Replies: **`PASS`** or **`FAIL`**, four ASCII bytes (0x08002740 reads either
`PASS` at 0x0800347A or `FAIL` at 0x0800347E and writes 4 bytes to the port).

The host must not write into `0x08000000-0x08003FFF`: the bootloader rejects
those records itself (`0x08000C30`: `cmp addr, #0x08000000`, `cmp addr,
#0x08003FFF` → FAIL path at 0x08000C44 → PASS).

## 3. Programming: the `E2` payload is a raw `.icf` record

The handler at `0x08000BE0` is exactly the validator described in `ra89r.py`,
but fed from the frame buffer at offset 5 (i.e. right after `FE FE EE EF E2`):

```
length  = (buf[5] ^ 0x66) << 8 | (buf[6] ^ 0x66)      0x08000C88
decrypt header, payload and check byte with the per-record key   0x080009FC
sum(buf[5 .. 5+length+6]) must be 0                   0x08000BF4..0x08000C0A
address = buf[7]<<24 | buf[8]<<16 | buf[9]<<8 | buf[10]          0x08000C0C
          (i.e. header[2..4] scaled by 0x100 — the .icf address field)
```

So the payload is the **raw record bytes** — the same bytes `ra89r.py` parse
(header, payload, check byte) — 6-byte header first.  `ra89r.py mkicf` produces
such a file from a flat image.

Writing (`0x0800266C`):

* FLASH unlock via `0x08000B8C` (KEYR writes);
* **2048-byte pages** (`page = (addr - 0x08000000) >> 11`, `offset = addr & 0x7FF`);
* the bootloader erases the page and merges: if any existing byte in the target
  range is not `0xFF` it copies the current page into a RAM buffer, patches in
  the new bytes and programs the whole 2048-byte page — so **partial-page
  records are fine**, and a full-image flash (as the CPS does) always works;
* each record is answered individually with `PASS`/`FAIL`.

## 4. Handshake and the identity announcement

After the `E0` frame the bootloader answers 19 bytes:

```
FE FE EF EE E1 <5 bytes> <4 bytes> <4 bytes> FD      (0x08003138..0x0800319E)
```

`FE FE EF EE E1` is matched by the updater (`ConHandAck`), then it decodes the
next 6 bytes as a hex string and compares them with the *last line* of the
update file — i.e. the announcement carries a model/version identity.  The last
four bytes of the announcement are the ASCII characters `0000` (0x08003186
stores `0x30` four times), so the meaningful identity is the first 9 bytes.

The updater's own entry frame is `FE FE EE EF E0 55 50 38 36 30 30 42 59 41 52 52
FD` = `E0` + `UP8600BYARR` (0x080004AC in the bootloader compares part of that
string).  `tools/ra89r_flash.py probe` sends exactly this frame and prints the
identity.

## 4b. What `EXIT` does, and whether our firmware will start

`EXIT` (0x08000B04) checks that the frame is exactly 10 bytes and that the
payload is the ASCII `EXIT`, answers PASS/FAIL, sets a flag, waits 100 ms and
then writes `SCB->AIRCR = 0x05FA0000 | (PRIGROUP & 0x700) | 0x04` — a
**SYSRESETREQ**, i.e. a full system reset (0x08000B44-0x08000B64).  So the radio
reboots when the host finishes.

On reset the bootloader decides whether to run the application (0x08003340ff):

```
r0 = *(app_base)          ; 0x08004000
r0 &= mask
if (r0 == 0x20000000) {   ; the app's initial stack pointer must look like SRAM
    msp = *(app_base + 0) ; vector[0]
    blx  *(app_base + 4)  ; vector[1] = reset handler
}
```

The trampoline's own validity test is only that the application's first vector
word is a `0x2000xxxx` SRAM address: no header, signature or checksum is
required, and our image starts with `SP = 0x20010000`.  What the trampoline does
*not* decide is whether to run the app at all -- for a cold start that is the
update-mode request byte plus the two key pins, and the `EXIT` handler resets the
chip, so a flash is always followed by that gate: §4c.

## 4c. The byte at 0x0805FFF0 is the *update-mode request* (0xFF runs the app)

This section used to call that byte an "application-valid marker" that had to
hold 0x11 for the radio to boot.  That was **backwards**, and the radio says so
plainly: our firmware runs with the byte at 0xFF.  The corrected reading:

* **0xFF** -- normal.  The bootloader starts the application.
* **0x11** -- "enter update mode on the next reset".  The bootloader enters
  update mode and then **consumes** the request by writing 0xFF back.

The reset vector leads straight into that decision: 0x08000144 runs
`SystemInit` and jumps to the stub at 0x08000130, which loads SP (`0x20003190`)
and jumps to **0x0800330C**.  So every reset -- power-on, pin, and the `EXIT`
handler's `SYSRESETREQ` (§4b) -- executes 0x08003318-0x080033AE:

```
0x0800331E  memcpy(0x20000004, 0x0805FFF0, 1)     /* flash byte -> RAM copy */
0x08003322  flag (RAM 0x20000005) = 0
0x0800332C  if (RAM byte == 0x11) flag = 2         /* flag = "update was asked" */
0x08003336  if ((GPIOB.IDR & 0x200) == 0          /* PB9 low ... */
0x08003342   && (GPIOA.IDR & 0x004) == 0)         /* ... and PA2 low */
0x08003354      bl 0x08000564                     /* update mode, byte ignored */
0x0800334E  else if (flag == 0)
0x0800335A      msp = *(0x08004000);              /* run the app ... */
0x08003354  else  bl 0x08000564                   /* ... unless 0x11 asked for it */
```

Read off that listing, not inferred:

* `cbz r0, 0x0800335A` at 0x08003352 branches to the **application launch** when
  the flag is 0 -- i.e. when the byte is *not* 0x11.  The launch trampoline (SP
  sanity check, `msr msp`, `blx`) is at 0x0800335A; the copy at 0x08003382 is
  what runs when 0x08000564 returns, and it tests nothing at all.
* With PB9 and PA2 both low the radio goes to update mode whatever the byte says:
  that is the key combination `firmware/FLASHING.md` §6 is still missing.  A
  released pin reads high (a normal power-on, nothing pressed, runs the
  application); the level convention is an inference from that, not from the
  code.
* 0x0805FFF0 is referenced in exactly **two** places in the 16 KiB bootloader:
  the read above, and the write at 0x08000566-0x08000578, which stores 0xFF into
  the RAM copy and flashes it -- but only when the request was set.  So a power
  cut during an update leaves the radio in the bootloader: recoverable, never
  half-flashed.

**Who sets 0x11.**  The stock application, from its PC command handler at
0x08015710: it compares 5 received bytes against `"Reset"` (the string lives at
0x080248A5) and then looks at a command byte -- `'0'` stores 0x11 at 0x0805FFF0
through its flash driver (`0x080190C0`) and falls into the same `SYSRESETREQ`
tail as the bootloader (0x0801576C).  That is how the CPS reboots a *running*
radio into the bootloader; the key combination is the manual equivalent.
Nothing anywhere validates the application: the bootloader's only test of the
image is the trampoline's SP sanity check.

**How flashing ends up, either way.**  After `EXIT` the chip resets and runs the
above.  With the flasher's default write (`0x11`, as `tools/ra89r_flash.py` has
always done, and what the CPS's `Reset`+`'0'` does) the reset enters update mode,
finds no host, consumes the request and then reaches the 0x08003382 trampoline,
which starts the application anyway; with `--no-valid-marker` the reset sees
`0xFF` and starts the application immediately.  Both end with **0xFF in flash and
our firmware running**, which is exactly what the radio reports.  The write is
therefore not needed to flash -- it only routes the first boot through the
bootloader once more.

**The old confusion, for the record.**  The earlier version of this section read
that `cbz` the wrong way round and concluded that 0x11 was required to start the
app, that the flashing tool had to write it, and that a cleared byte meant a
black screen.  All three are inverted.  What should have caught it immediately:
our own firmware answering on the console while printing `0xFF`.

## 4d. The bootloader drives the same panel

While in update mode the bootloader displays `Update....` on the LCD, which
makes its own display driver a *known-working* reference for this glass: init at
`0x08002440` (standard ST7565 commands only), byte writer at `0x080024D0`
(CS PA11, SCLK PA8, data PB15, MSB first), command/data at `0x0800254C` /
`0x08002572` (A0/DC PA10) and the page/column builder at `0x08000CC8`
(page `0xB0|n`, column `0x10|hi`, `lo`, with the `+4` offset and a clamp at
127).  All of it matches `ra89r_lcd.md`, and it proves the eight extra init
bytes the stock *application* sends are not needed.

## 5. Baud rates and speed

### Baud table

`E5` + `BAUDRATE` + **two ASCII digits** (the updater sends
`StrToAscii(SelectedIndex)` zero-padded to 2) selects from a table in the
bootloader (0x080004AC, `sub r0, #0x30` converts the digit):

| index | baud |
|---|---|
| 0 | 9600 |
| 1 | 19200 |
| 2 | 38400 |
| 3 | 56000 |
| 4 | 57600 |
| 5 | 115200 |
| 6 | 256000 |
| 7 | 512000 |
| 8 | 1024000 |

After the command the bootloader waits 1000 ms (0x08000550) before it is ready
again, so the host should pause briefly after switching.

### What actually limits the speed

| stage | cost | source |
|---|---|---|
| serial transfer | `bytes * 10 / baud` — **the main lever** | 8N1 framing |
| page erase | 5 ms per 2048-byte page | datasheet, `tERASE` |
| page program | 1.5 ms per page | datasheet, `tPROG` |
| per-record ack | one PASS/FAIL round trip (~4 bytes + USB latency) | protocol design |

For the 146 KB stock image that is 72 pages -> **~0.5 s of unavoidable
erase/program work**; everything else is serial time, i.e.
12.7 s at 115200, 5.7 s at 256000, 3.8 s at 384000.

The ceiling comes from the bootloader's clock: it runs on the **8 MHz reset
clock** (it reads `RCC_CFGR` but never writes it, and never sets `OVER8`), so
with 16x oversampling the USART tops out near 500 kbaud.  **256000 has exactly
0% divisor error** at 8 MHz; 384000 is off by ~0.1%; 512000 and up need more
than 8 MHz PCLK and are not usable.

Because the `BAUDRATE` command is not acknowledged, `tools/ra89r_flash.py` does
not trust it: after sending it, the host **sweeps every rate in the table** and
keeps the fastest one that answers a harmless `E0` ping.  A wrong guess
therefore cannot lose the session — verified against the simulator (`E0` pings
deliberately dropped for 2 s: the tool settled on 115200 and finished).

## 6. Speeding up flashing (measured)

`tools/ra89r_bootloader_sim.py` is a test double that speaks this protocol on a
PTY and emulates the page erase/program timing above, so the host side can be
exercised end to end without the radio:

```sh
python3 tools/ra89r_bootloader_sim.py --pty /tmp/ra89r-pty &
python3 tools/ra89r_flash.py --port $(cat /tmp/ra89r-pty) flash image.icf
```

It verifies the record encoding the bootloader itself enforces (header length,
byte-sum check, refused addresses), and its options let the error paths be
tested too: `--fail-first-n N` (transient failures -> resync + retry),
`--mutate-every N` (persistent checksum failures -> abort),
`--mute-after-baud S` (host guessing the wrong baud).  A PTY has no baud rate,
so it cannot validate the rate ceiling itself.

Measured with the simulator (radio-side work + protocol overhead, no serial
time):

| image | records | time |
|---|---|---|
| our bring-up firmware (6.9 KB) | 4 | 0.03 s |
| stock firmware (146 KB) | 72 | 0.5 s |

What changed in the tool, in order of impact:

1. **Ack handling bug (fixed).**  `PASS`/`FAIL` carry no `0xFD`, so the old
   `read_until(FD)` waited out the full 1.5 s timeout after *every* record:
   ~108 s of pure waiting for a 72-record image.  Acks are now read as exactly
   four bytes with a deadline, costing microseconds.
2. **Baud.**  Default is now `--baud auto` (fastest rate that answers) instead
   of a fixed 115200: 2-3x on real hardware, and it self-documents the ceiling.
3. **`--window N`** (default 1) sends N records before draining acks, to hide
   the ~7 ms/record the bootloader spends erasing.  Only worth it at high baud
   (a 2 KB record takes ~80 ms to send at 256000) and unvalidated on hardware.
4. Not improvable: the per-page erase/program (~0.5 s for 146 KB) and the
   `BAUDRATE` settle wait (1 s, once).

Realistic total for the stock image: **~4-6 s** with `--baud auto` (was ~120 s
with the old wait behaviour at 115200).

## 7. Flashing with this repository's tools

```sh
# wrap the firmware image into records the bootloader accepts
python3 ra89r.py mkicf firmware/build/Release/ra89r_fw.bin firmware/build/Release/ra89r_fw.icf

# is the radio in update mode?  (send the handshake, print the identity)
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe

# program it
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash firmware/build/Release/ra89r_fw.icf

# put the stock firmware back (same command, stock file)
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash FIRMWARE_RA89R_20260203_V49.icf
```

`--dry-run` prints every frame without touching the port, `--baud keep` (or
`--baud-index 0`) stays at 9600, `--window N` pipelines acks, `--erase-first`
sends the `E1`/`CLEAR` command, and records at `0x08000000-0x08003FFF` are
refused unless `--allow-bootloader-region` is given.

## 8. Open points

1. **How to enter update mode** (which key is held at power-on) is not encoded in
   the protocol; the CPS shows the prompt to the user.  Not recovered here.
2. **`E1`/`CLEAR` semantics** (what exactly it erases) and **`E3`/`READ05`**
   (read-out) were not exercised; the handlers are at 0x080005DC and 0x08002740.
3. ~~The CPS's own update-file flavour~~ — **resolved.**  The updater builds a
   *hex string* (`"FEFEEEEFE2" + <the .icf line> + "FD"`) and `SendDAtaPro`
   hex-decodes it before writing to the port, so the bytes on the wire are
   `FE FE EE EF E2 <raw record bytes> FD` -- exactly what the bootloader parses
   and exactly what `tools/ra89r_flash.py` sends.  The distributor update file
   `Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf` confirms the container: same
   ASCII-hex CR-separated records, same `0x66` baseline, records from
   `0x08004000` in 2048-byte steps, and `ra89r.py verify` accepts it
   (72 records).  The only dead branch is `CommunicationStep == 2`
   (`StrToAscii` on the first line), which the live path never reaches.
4. **Identity bytes** (the 9 meaningful ones) are read from a flash table at
   0x080031B4-0x080031BC and were not decoded; they are almost certainly the
   model/version string the CPS matches against.
5. The announcement is produced by the *bootloader*.  The stock *application*
   answers the same `E0` handshake with a model id instead (the updater's
   `AckDataChkPro` handles values such as `2900`, `3100`, `8500`), which is how
   the CPS warns that the file does not match the radio before you even enter
   update mode.
