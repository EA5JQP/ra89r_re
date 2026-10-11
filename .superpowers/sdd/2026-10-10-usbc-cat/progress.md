# SDD ledger — usbc CAT
- Radio side: App/cat.{c,h} (pure FT-817 CAT parser, host-tested), App/cat_radio.c (actions).
- main.c: auto-detect USART1 bytes; ENABLE_CAT gating (CAT-only; console when off).
- tools/test_cat.c (15 checks), tools/look4sat_cat.py, docs/ra89r_cat.md.
- Ruling: auto-detect rather than a console mode switch (user) and CAT-only under ENABLE_CAT (finished firmware has no console) — cost if wrong: console absent in the CAT build; build with ENABLE_CAT off for it.
- Ruling: reuse Look4Sat's FT-817 CAT (user chose A) so the phone-side change is transport-only — cost if wrong: radio CAT parser must match Look4Sat's byte shapes (it does, per Ft817CatProtocol).
- Ruling: baud left at the console's 115200 while the tool defaults to 9600 — must be settled on the radio; documented.
- Radio gate pending: no on-radio validation yet.
