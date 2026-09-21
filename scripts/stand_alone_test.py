#!/usr/bin/env python3
"""WT32-ETH01 STANDALONE two-radio UDP bandwidth test.

Host stamps 802.11 MPDUs (tools/mpdu.py), injects into each radio's
set_upstream_tx, and receives forwarded MPDUs on set_upstream_rx. Buses
isolate A→B vs B→A. No winject-manager.

    python3 scripts/stand_alone_test.py --a 192.168.253.9 --b 192.168.253.14
    python3 scripts/stand_alone_test.py --modulation OFDM_24M --no-cca
    python3 scripts/stand_alone_test.py --all --test-integ --test-ab
"""

from __future__ import annotations

import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import bw_test as bw

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)


def main() -> int:
    argv = [a for a in sys.argv[1:] if a not in ("--tcp", "--udp", "--direct")]
    sys.argv = [sys.argv[0], "--direct", *argv]
    return bw.main()


if __name__ == "__main__":
    raise SystemExit(main())
