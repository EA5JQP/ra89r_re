# f4hwn -- the F4HWN UV-K1/K5V3 snapshot, fresh RA89R port

Snapshot of the upstream F4HWN tree
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`, PY32F071), pinned at
`dc220aa92da0d54c9cf88360c37cc3787128e889` ("Update apps", 2026-10-17).

Policy (see `docs/superpowers/plans/2026-10-26-f4hwn-fresh-port.md`):

* `App/` is taken **as-is**. Every difference from upstream must be a
  classified, commented `Port adaptation` block inside the file it belongs to
  -- no drive-by edits.
* `App/external/CMSIS_5/` is not carried (50 MB, the K5V3 vendors a whole
  CMSIS); the RA89R build uses `PY32F4xx_Firmware/` (the repo's SDK).
* `compat/` holds the only allowed include-level shims:
  `py32f0xx.h` (device header name) and `py32f071_ll_<x>.h` -> the PY32F403
  SDK's LL headers of the same name. Nothing else goes there.
* The board facts (pins, clock, panel, keypad, audio, speaker path, squelch
  table) come from the **validated register-level drivers in the old port**
  (`firmware/App/driver/`, `firmware/App/board*.h`) -- copied over or called
  out as patches where the upstream file stays.

Build: the `f4hwn_fw` target in `firmware/CMakeLists.txt`
(`cmake --preset <f4hwn preset>`; output `build/f4hwn/f4hwn_fw.icf`).
