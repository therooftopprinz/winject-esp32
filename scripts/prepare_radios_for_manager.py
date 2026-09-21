#!/usr/bin/env python3
"""Configure STANDALONE radio mode/domain + upstream for manager bw_test.

Always applied: set_mode STANDALONE, set_domain, sut/sur bind.
Optional (only if flagged): --channel, --modulation, --power, --cca/--no-cca.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import bw_test as bw  # noqa: E402

FWD_A = 9210
FWD_B = 9220
DEFAULT_DOMAIN = bw.DEFAULT_DOMAIN
INJECT = bw.INJECT_PORT
# Same-domain radios not in --a/--b must not forward to manager ports (dupes).
BENCH_CLEAR_RX = (
    "192.168.253.9",
    "192.168.253.11",
    "192.168.253.12",
    "192.168.253.14",
)


def clear_stale_forwarders(active: set[str], quiet: bool) -> None:
    for ip in BENCH_CLEAR_RX:
        if ip in active:
            continue
        try:
            bw.console(ip, ["unset_upstream_rx"], quiet=quiet, timeout=2.0)
        except OSError:
            pass


def main() -> int:
    p = argparse.ArgumentParser(description="prepare radios for manager bw_test")
    p.add_argument("--a", default="192.168.253.11")
    p.add_argument("--b", default="192.168.253.12")
    p.add_argument("--host", default="")
    p.add_argument(
        "--domain",
        default=DEFAULT_DOMAIN,
        help=f"shared air domain, hex 1..ffff (default {DEFAULT_DOMAIN})",
    )
    p.add_argument(
        "--channel",
        type=int,
        default=None,
        help=f"set_channel {bw.CHANNEL_MIN}-{bw.CHANNEL_MAX} "
        f"(omit to keep existing; 14 is 802.11b-only)",
    )
    p.add_argument(
        "--modulation",
        default=None,
        help="set_modulation (omit to keep existing; DSSS/CCK required on channel 14)",
    )
    p.add_argument(
        "--power",
        type=int,
        default=None,
        help="set_tx_power (omit to keep existing)",
    )
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "--cca",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="set_cca_enabled; omit to keep existing",
    )
    args = p.parse_args()
    if args.channel is not None and not bw.channel_ok(args.channel):
        raise SystemExit(f"--channel must be {bw.CHANNEL_MIN}-{bw.CHANNEL_MAX}")
    if args.modulation is not None and not bw.modulation_ok_for_channel(
        args.modulation, args.channel
    ):
        raise SystemExit(
            "channel 14 rejects OFDM/MCS; use a DSSS/CCK --modulation "
            f"(got {args.modulation})"
        )
    host = args.host or bw.detect_host(args.a)
    quiet = not args.verbose
    domain = bw.fmt_domain(args.domain)

    def configure_radio(ip: str, forward: int) -> bool:
        phy_cmds: list[str] = []
        if args.channel == 14:
            if args.modulation is not None:
                phy_cmds.append(f"set_modulation {args.modulation}")
            phy_cmds.append(f"set_channel {args.channel}")
        else:
            if args.channel is not None:
                phy_cmds.append(f"set_channel {args.channel}")
            if args.modulation is not None:
                phy_cmds.append(f"set_modulation {args.modulation}")
        if args.power is not None:
            phy_cmds.append(f"set_tx_power {args.power}")
        if args.cca is not None:
            phy_cmds.append(f"set_cca_enabled {1 if args.cca else 0}")
        cmds = [
            "wifi_bench_stop",
            "set_mode STANDALONE",
            "set_inject_sink wifi",
            *phy_cmds,
            f"set_domain {domain}",
            *bw.upstream_bind_cmds(host, INJECT, forward, "b2", "a1"),
        ]
        try:
            replies = bw.console(ip, cmds, quiet=quiet, timeout=5)
        except OSError as err:
            print(f"{ip}: console failed: {err}")
            return False
        if not bw.replies_ok(replies):
            print(f"{ip}: configure failed (power-cycle ESP32 if console is wedged)")
            return False
        return True

    inject_tune = (
        "set_inject_tune flush_batch=8 emac_gap_ticks=0 "
        "max_in_flight=6 staging_margin=4"
    )

    def apply_inject_tune(ip: str) -> bool:
        try:
            replies = bw.console(ip, [inject_tune], quiet=quiet, timeout=5)
        except OSError as err:
            print(f"{ip}: inject_tune console failed: {err}")
            return False
        if not bw.replies_ok(replies):
            print(f"{ip}: inject_tune failed")
            return False
        return True

    clear_stale_forwarders({args.a, args.b}, quiet)
    ok_a = configure_radio(args.a, FWD_A)
    ok_b = configure_radio(args.b, FWD_B)
    if ok_a:
        ok_a = apply_inject_tune(args.a)
    if ok_b:
        ok_b = apply_inject_tune(args.b)
    if not (ok_a and ok_b):
        return 1
    ch = "unchanged" if args.channel is None else str(args.channel)
    mod = "unchanged" if args.modulation is None else args.modulation
    pwr = "unchanged" if args.power is None else str(args.power)
    if args.cca is None:
        cca = "unchanged"
    else:
        cca = "enabled" if args.cca else "disabled"
    print(
        f"ok domain={domain} channel={ch} modulation={mod} power={pwr} cca={cca} "
        f"inject={INJECT} forward A={FWD_A} B={FWD_B} host={host}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
