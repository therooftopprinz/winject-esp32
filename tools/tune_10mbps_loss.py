#!/usr/bin/env python3
"""Sweep radio inject tune + optional manager pacing at a fixed paced host offer.

Uses set_inject_tune on older firmware or set_wifi_tx_tune on upstream-refactor FW.
Runs manager_bw_test.sh (A→B only) per cell and prints loss + eth-stage drop %.

Default offer is 10 Mbps; use --kbps 15000 for the 15 Mbps bench profile sweep.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import bw_test as bw  # noqa: E402

# Bench winner (2026-09-21, inject FW): use prepare --profile 10mbps or this spec.
BENCH_10MBPS_INJECT = (
    "flush_batch=6 emac_gap_ticks=0 max_in_flight=6 staging_margin=4"
)
BENCH_10MBPS_WIFI = "burst_size=6 burst_gap_us=0 max_in_flight=6"
BENCH_15MBPS_INJECT = (
    "flush_batch=8 emac_gap_ticks=0 max_in_flight=8 staging_margin=4"
)
BENCH_15MBPS_WIFI = "burst_size=8 burst_gap_us=0 max_in_flight=8"

DEFAULT_TUNES = [
    ("default", None),
    ("bench_10mbps", BENCH_10MBPS_INJECT),
    ("emac_gap1_mif4", "flush_batch=4 emac_gap_ticks=1 max_in_flight=4 staging_margin=4"),
    ("batch4_gap0", "flush_batch=4 emac_gap_ticks=0 max_in_flight=6 staging_margin=4"),
    ("batch4_mif4_tick3", "flush_batch=4 emac_gap_ticks=0 max_in_flight=4 staging_margin=4"),
]

TUNES_15MBPS = [
    ("default", None),
    ("bench_15mbps", BENCH_15MBPS_INJECT),
    ("bench_10mbps", BENCH_10MBPS_INJECT),
    ("b8g0_m6", "flush_batch=8 emac_gap_ticks=0 max_in_flight=6 staging_margin=4"),
    ("b6g0_m6", BENCH_10MBPS_INJECT),
]


def detect_tune_cmd(ip: str) -> str:
    reply = "\n".join(bw.console(ip, ["help"], quiet=True, timeout=3))
    if "set_wifi_tx_tune" in reply or "swtt" in reply:
        return "wifi"
    return "inject"


def apply_tune(ip: str, spec: str | None, mode: str) -> None:
    if spec is None:
        return
    if mode == "wifi":
        if spec == BENCH_10MBPS_INJECT:
            spec = BENCH_10MBPS_WIFI
        elif spec == BENCH_15MBPS_INJECT:
            spec = BENCH_15MBPS_WIFI
    if mode == "wifi":
        cmd = f"set_wifi_tx_tune {spec}"
    else:
        cmd = f"set_inject_tune {spec}"
    if not bw.replies_ok(bw.console(ip, [cmd], quiet=True, timeout=5)):
        raise RuntimeError(f"{ip}: {cmd} failed")


def run_bw_test(
    a: str,
    b: str,
    channel: int,
    offer_kbps: int,
    mgr_tick: int | None = None,
    profile: str | None = None,
) -> str:
    cmd = [
        str(ROOT / "scripts/manager_bw_test.sh"),
        "--a",
        a,
        "--b",
        b,
        f"--channel",
        str(channel),
        "--test-ab",
        "--kbps",
        str(offer_kbps),
    ]
    if profile:
        cmd.extend(["--profile", profile])
    if mgr_tick is not None:
        cmd.extend(["--max-data-per-tick", str(mgr_tick)])
    proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    return proc.stdout + proc.stderr


def parse_result(text: str) -> tuple[float, float, float | None]:
    m = re.search(
        r"A->B sent \d+ recv \d+\s+([\d.]+) kbps\s+loss ([\d.]+)%",
        text,
    )
    if not m:
        raise ValueError("no A->B result")
    good = float(m.group(1))
    loss = float(m.group(2))
    eth = None
    st = re.search(
        r"mgr_wifi→radio_udp\s+(\d+) →\s+(\d+)\s+drop\s+(\d+)",
        text,
    )
    if st:
        left = int(st.group(1))
        drop = int(st.group(3))
        eth = 100.0 * drop / left if left else 0.0
    return good, loss, eth


def write_manager_cfg(
    src: Path, max_data_per_tick: int, max_rate_kbps: int
) -> Path:
    text = src.read_text()
    lines = []
    for line in text.splitlines():
        if line.strip().startswith("winject.max_data_per_tick"):
            continue
        if line.strip().startswith("winject.max_rate_kbps"):
            continue
        lines.append(line)
    insert_at = 0
    for i, line in enumerate(lines):
        if line.strip().startswith("winject.max_rate_kbps"):
            insert_at = i
            break
        if line.strip().startswith("winject.stats_sec"):
            insert_at = i + 1
    lines.insert(insert_at, f"winject.max_rate_kbps = {max_rate_kbps}")
    lines.insert(insert_at + 1, f"winject.max_data_per_tick = {max_data_per_tick}")
    tmp = Path(tempfile.mkdtemp(prefix="tune10_"))
    (tmp / "a.cfg").write_text("\n".join(lines) + "\n")
    (tmp / "b.cfg").write_text((ROOT / "configuration/winject-tests/lat_udp_b.cfg").read_text())
    return tmp


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="radio-em0")
    p.add_argument("--b", default="radio-em1")
    p.add_argument("--channel", type=int, default=13)
    p.add_argument("--kbps", type=int, default=10000, help="paced host offer (default 10000)")
    p.add_argument("--quick", action="store_true", help="first 3 tune cells only")
    p.add_argument(
        "--mgr-tick",
        type=int,
        default=None,
        help="append winject.max_data_per_tick on manager A (via manager_bw_test.sh)",
    )
    args = p.parse_args()

    mode = detect_tune_cmd(args.a)
    cells = (TUNES_15MBPS if args.kbps == 15000 else DEFAULT_TUNES)
    cells = cells[:3] if args.quick else cells
    print(
        f"tune mode={mode}  offer={args.kbps} kbps  A→B only  channel={args.channel}\n"
    )
    print(f"{'name':<18} {'Mbps':>7} {'loss%':>7} {'eth_drop%':>9}")
    print("-" * 44)

    best = ("", 999.0, 0.0)
    for name, spec in cells:
        try:
            apply_tune(args.a, spec, mode)
            apply_tune(args.b, spec, mode)
        except RuntimeError as err:
            print(f"{name:<18} SKIP {err}")
            continue
        prof = "15mbps" if name == "bench_15mbps" else None
        if name == "bench_10mbps" and args.kbps == 15000:
            prof = "10mbps"
        out = run_bw_test(
            args.a, args.b, args.channel, args.kbps, args.mgr_tick, prof
        )
        try:
            good, loss, eth = parse_result(out)
        except ValueError:
            print(f"{name:<18} FAIL parse")
            continue
        eth_s = f"{eth:.1f}" if eth is not None else "n/a"
        print(f"{name:<18} {good/1000:7.2f} {loss:7.1f} {eth_s:>9}")
        if loss < best[1]:
            best = (name, loss, good)

    print(f"\nbest loss: {best[0]}  loss={best[1]:.1f}%  goodput={best[2]/1000:.2f} Mbps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
