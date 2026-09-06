#!/usr/bin/env python3
"""Configure STANDALONE domain/bus upstreams for manager TCP bw_test.

Binds must match configuration/winject-tests/bw_{a,b}.cfg:
  Pair 1 (A→B): A sut b2 / sur a1, B sut a1 / sur b2
  Pair 2 (B→A): A sut c3 / sur d4, B sut d4 / sur c3
Inject ports 9000/9010; forward bases 9210 (A) / 9220 (B).
"""

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
DEFAULT_DOMAIN = bw.DEFAULT_DOMAIN
# Pair 1: A→B. Pair 2: B→A. Must match bw_a.cfg / bw_b.cfg.
PAIR1_AB = bw.BUS_AB
PAIR1_BA = bw.BUS_BA
PAIR2_AB = "c3"
PAIR2_BA = "d4"


def main() -> int:
    p = argparse.ArgumentParser(description="prepare radios for manager TCP bw_test")
    p.add_argument("--a", default="192.168.253.11")
    p.add_argument("--b", default="192.168.253.12")
    p.add_argument("--host", default="")
    p.add_argument(
        "--domain",
        default=DEFAULT_DOMAIN,
        help=f"shared air domain, hex 1..ffff (default {DEFAULT_DOMAIN})",
    )
    p.add_argument("--channel", type=int, default=1, help="set_channel (default 1)")
    p.add_argument(
        "--modulation",
        default="OFDM_24M",
        help="set_modulation (default OFDM_24M)",
    )
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "--cca",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="set_cca_enabled on both radios (default: on)",
    )
    args = p.parse_args()
    if args.channel < 1 or args.channel > 13:
        raise SystemExit("--channel must be 1-13")
    host = args.host or bw.detect_host(args.a)
    quiet = not args.verbose
    cca = 1 if args.cca else 0
    domain = bw.fmt_domain(args.domain)
    pair1_ab = bw.fmt_bus(PAIR1_AB)
    pair1_ba = bw.fmt_bus(PAIR1_BA)
    pair2_ab = bw.fmt_bus(PAIR2_AB)
    pair2_ba = bw.fmt_bus(PAIR2_BA)

    # (bus_tx, bus_rx, inject, forward) per upstream
    a_binds = [
        (pair1_ab, pair1_ba, 9000, FWD_A),
        (pair2_ab, pair2_ba, 9010, FWD_A + 1),
    ]
    b_binds = [
        (pair1_ba, pair1_ab, 9000, FWD_B),
        (pair2_ba, pair2_ab, 9010, FWD_B + 1),
    ]

    def configure_radio(ip: str, binds: list[tuple[str, str, int, int]]) -> bool:
        cmds = [
            "set_mode STANDALONE",
            f"set_channel {args.channel}",
            f"set_modulation {args.modulation}",
            "set_tx_power 20",
            f"set_cca_enabled {cca}",
            f"set_domain {domain}",
        ]
        for bus_tx, bus_rx, inject, forward in binds:
            cmds.extend(
                bw.upstream_bind_cmds(host, inject, forward, bus_tx, bus_rx)
            )
        try:
            replies = bw.console(ip, cmds, quiet=quiet, timeout=5)
        except OSError as err:
            print(f"{ip}: console failed: {err}")
            return False
        if not bw.replies_ok(replies):
            print(f"{ip}: configure failed (power-cycle ESP32 if console is wedged)")
            return False
        return True

    print(
        f"prepare radios domain={domain} ch={args.channel} "
        f"mod={args.modulation} cca={cca} "
        f"pair1 {pair1_ab}/{pair1_ba} pair2 {pair2_ab}/{pair2_ba}"
    )
    ok = configure_radio(args.a, a_binds) and configure_radio(args.b, b_binds)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
