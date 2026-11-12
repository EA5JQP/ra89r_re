# Retevis RA89R running ra89r_fw (the F4HWN UV-K1 port) -- CHIRP driver.
#
# The firmware serves the K5/K1 serial programming protocol and the F4HWN
# v6.2.0 virtual EEPROM layout (firmware/App/k5prog.c, firmware/App/k1_view.c,
# docs/ra89r_chirp.md), so this is the published F4HWN v6.2.0 CHIRP driver
# with the RA89R's serial speed: the console/CAT/programming port runs at
# 115200, not the K5's 38400.
#
# Install next to f4hwn_v6_2_0.py as chirp/drivers/ra89r_f4hwn.py (desktop
# CHIRP: "Load module"; webchirp: see tools/webchirp/README.md).
#
# SPDX-License-Identifier: GPL-3.0-or-later  (as the CHIRP driver it extends)

from chirp import chirp_common, directory
from chirp.drivers import f4hwn_v6_2_0 as _f4hwn


@directory.register
class RA89RF4HWNRadio(_f4hwn.UVK5RadioEgzumer):
    """Retevis RA89R (ra89r_fw, F4HWN v6.2.0 layout)"""
    VENDOR = "Retevis"
    MODEL = "RA89R (F4HWN port)"
    BAUD_RATE = 115200

    @classmethod
    def get_prompts(cls):
        rp = chirp_common.RadioPrompts()
        rp.experimental = (
            "This driver talks to the ra89r_fw custom firmware on a Retevis "
            "RA89R, not to the stock firmware.\n\n"
            "Download and keep an image before changing anything. Channels, "
            "names, scan lists, VFOs and settings are written; the "
            "calibration area is read-only on this radio (an upload with "
            "'upload calibration' ticked leaves it unchanged).")
        rp.pre_download = (
            "1. Turn the radio on (ra89r_fw running).\n"
            "2. Connect the programming cable (Kenwood jack, 115200).\n"
            "3. Click OK to download.\n")
        rp.pre_upload = (
            "1. Turn the radio on (ra89r_fw running).\n"
            "2. Connect the programming cable (Kenwood jack, 115200).\n"
            "3. Click OK to upload. The radio restarts when it is done.\n")
        return rp
