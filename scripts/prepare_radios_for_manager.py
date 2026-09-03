#!/usr/bin/env python3
"""Configure STANDALONE upstreams for manager TCP bw_test (fixed forward ports)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import bw_test as bw  # noqa: E402

# Manager forward_base per radio (must match configuration/winject-tests/bw_*.cfg).
FWD_A = 9210
FWD_B = 9220


def main() -> int:
    p = argparse.ArgumentParser(description="prepare radios for manager TCP bw_test")
    p.add_argument("--a", default="192.168.253.11")
    p.add_argument("--b", default="192.168.253.12")
    p.add_argument("--host", default="")
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "--cca",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="set_cca_enabled on both radios (default: on)",
    )
    args = p.parse_args()
    host = args.host or bw.detect_host(args.a)
    quiet = not args.verbose
    cca = 1 if args.cca else 0

    a_airports = [
        ("AA:BB:CC:22:22:33", 9000, FWD_A),
        ("AA:BB:CC:44:55:66", 9010, FWD_A + 1),
    ]
    b_airports = [
        ("22:22:33:AA:BB:CC", 9000, FWD_B),
        ("44:55:66:AA:BB:CC", 9010, FWD_B + 1),
    ]

    def configure_radio(ip: str, airports: list[tuple[str, int, int]]) -> bool:
        cmds = [
            "set_mode STANDALONE",
            "set_channel 1",
            "set_modulation OFDM_24M",
            "set_tx_power 20",
            f"set_cca_enabled {cca}",
        ]
        # Clear any stale upstream bindings for these airports first.
        for airport, _inject, _forward in airports:
            cmds.append(f"unset_upstream_rx {airport}")
            cmds.append(f"unset_upstream_tx {airport}")
        for airport, inject, forward in airports:
            cmds.append(f"set_upstream_rx {airport} {inject}")
            cmds.append(f"set_upstream_tx {airport} {host} {forward}")
        try:
            replies = bw.console(ip, cmds, quiet=quiet, timeout=5)
        except OSError as err:
            print(f"{ip}: console failed: {err}")
            return False
        if not bw.replies_ok(replies):
            print(f"{ip}: configure failed (power-cycle ESP32 if console is wedged)")
            return False
        return True

    ok = configure_radio(args.a, a_airports) and configure_radio(args.b, b_airports)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
