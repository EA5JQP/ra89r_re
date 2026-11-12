# webchirp / CHIRP for the RA89R (`ra89r_fw`)

The firmware speaks the UV-K5/K1 programming protocol and the F4HWN v6.2.0
memory layout (`docs/ra89r_chirp.md`).  The driver here is the published
F4HWN v6.2.0 CHIRP driver with the RA89R's serial speed (115200).

| file | what |
|---|---|
| `ra89r_f4hwn.py` | the driver: `Retevis` / `RA89R (F4HWN port)`, a subclass of `f4hwn_v6_2_0` |
| `webchirp-ra89r.patch` | adds it to webchirp (against `c92b414`): the driver file, its `python-sources.ts` entry (with its sha256), and its catalog row |
| `chirp_e2e.py` | the real driver against the firmware's code on a PTY, no radio needed |

## webchirp, locally

webchirp.org only serves the drivers it ships.  Until the RA89R entry is
upstream, run webchirp from a checkout with the patch:

```sh
git clone --recurse-submodules https://github.com/jasiek/webchirp
cd webchirp
git apply /path/to/ra89r_re/tools/webchirp/webchirp-ra89r.patch
npm install
npm run dev            # http://127.0.0.1:8000/?drivers=quansheng-unofficial
```

In the radio picker, choose **Retevis RA89R (F4HWN port)** (release "RA89R
ra89r_fw"), connect the programming cable, then **Load from radio**.  The
radio must be running `ra89r_fw`, not the stock firmware.

The patch changes `ra89r_f4hwn.py` by its sha256.  After editing the driver,
regenerate the patch (`git diff` in the patched checkout) so the hash in
`python-sources.ts` matches.  webchirp's own `tests/channels/f4hwn-releases.mjs`
pins the bundled set at six F4HWN releases, so an upstream PR also has to
extend that test.

## Desktop CHIRP

Copy `f4hwn_v6_2_0.py` (from webchirp's `web/python/extra_drivers/quansheng/`
or the F4HWN release) and `ra89r_f4hwn.py` into CHIRP's `chirp/drivers/`.
Alternatively, load both through CHIRP's developer *Load module* option:
`f4hwn_v6_2_0.py` first, then `ra89r_f4hwn.py`.

## The offline check

```sh
python3 tools/webchirp/chirp_e2e.py --chirp <chirp source tree> \
    --driver <path to f4hwn_v6_2_0.py>
```

It needs `pyserial` and `gcc`, and builds `firmware/tools/k5prog_sim.c`.
