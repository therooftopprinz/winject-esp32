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
# Bench inject FW (radio-em0→em1): see docs/inject-pacing-investigation.md
PROFILE_10MBPS_INJECT = (
    "flush_batch=6 emac_gap_ticks=0 max_in_flight=6 staging_margin=4"
)
PROFILE_10MBPS_WIFI = "burst_size=6 burst_gap_us=0 max_in_flight=6"
PROFILE_15MBPS_INJECT = (
    "flush_batch=8 emac_gap_ticks=1 max_in_flight=6 staging_margin=4"
)
PROFILE_15MBPS_WIFI = "burst_size=8 burst_gap_us=1000 max_in_flight=6"
PROFILE_WIFI_BY_NAME = {
    "10mbps": PROFILE_10MBPS_WIFI,
    "15mbps": PROFILE_15MBPS_WIFI,
}
PROFILE_INJECT_BY_NAME = {
    "10mbps": PROFILE_10MBPS_INJECT,
    "15mbps": PROFILE_15MBPS_INJECT,
}
# Same-domain radios not in --a/--b (offline bench spares only).
BENCH_CLEAR_RX = (
    "192.168.253.11",
    "192.168.253.12",
)


def clear_stale_forwarders(active: set[str], quiet: bool) -> None:
    for ip in BENCH_CLEAR_RX:
        if ip in active:
            continue
        try:
            bw.console(ip, ["unset_upstream_rx"], quiet=quiet, timeout=0.8)
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
    p.add_argument(
        "--profile",
        choices=tuple(PROFILE_INJECT_BY_NAME),
        default=None,
        help="apply radio TX pacing for a paced host offer (see docs/inject-pacing-investigation.md)",
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

    def apply_profile(ip: str, profile: str) -> bool:
        help_text = "\n".join(bw.console(ip, ["help"], quiet=quiet, timeout=3))
        if "set_wifi_tx_tune" in help_text or "swtt" in help_text:
            tune_cmd = f"set_wifi_tx_tune {PROFILE_WIFI_BY_NAME[profile]}"
        else:
            tune_cmd = f"set_inject_tune {PROFILE_INJECT_BY_NAME[profile]}"
        try:
            replies = bw.console(ip, [tune_cmd], quiet=quiet, timeout=5)
        except OSError as err:
            print(f"{ip}: profile tune failed: {err}")
            return False
        if not bw.replies_ok(replies):
            print(f"{ip}: {tune_cmd} failed")
            return False
        return True

    clear_stale_forwarders({args.a, args.b}, quiet)
    ok_a = configure_radio(args.a, FWD_A)
    ok_b = configure_radio(args.b, FWD_B)
    if not (ok_a and ok_b):
        return 1
    if args.profile is not None:
        if not (
            apply_profile(args.a, args.profile)
            and apply_profile(args.b, args.profile)
        ):
            return 1
    ch = "unchanged" if args.channel is None else str(args.channel)
    mod = "unchanged" if args.modulation is None else args.modulation
    pwr = "unchanged" if args.power is None else str(args.power)
    if args.cca is None:
        cca = "unchanged"
    else:
        cca = "enabled" if args.cca else "disabled"
    prof = args.profile or "none"
    print(
        f"ok domain={domain} channel={ch} modulation={mod} power={pwr} cca={cca} "
        f"profile={prof} inject={INJECT} forward A={FWD_A} B={FWD_B} host={host}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
