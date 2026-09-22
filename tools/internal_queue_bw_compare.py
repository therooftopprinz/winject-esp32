#!/usr/bin/env python3
"""Compare passive paced vs internal unified inject queue on manager bw_test.

Runs A→B and B→A for each mode (default OFDM_24M, no-CCA, lat_udp configs).
Requires flashed firmware with queue_mode=internal support and both radios up.

Example:
  python3 tools/internal_queue_bw_compare.py --a 192.168.253.9 --b 192.168.253.14
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def inject_tune(ip: str, tune: str) -> None:
    sys.path.insert(0, str(ROOT / "tools"))
    import bw_test as bw

    cmd = f"set_inject_tune {tune}"
    if not bw.replies_ok(bw.console(ip, [cmd], quiet=True)):
        raise RuntimeError(f"{ip}: {cmd} failed")


@dataclass
class LegResult:
    label: str
    kbps: float
    loss_pct: float


def run_bw(
    a: str,
    b: str,
    host: str,
    manager: Path,
    test_ab: bool,
    test_ba: bool,
) -> list[LegResult]:
    conf_a = ROOT / "configuration/winject-tests/lat_udp_a.cfg"
    conf_b = ROOT / "configuration/winject-tests/lat_udp_b.cfg"
    work = Path(tempfile.mkdtemp(prefix="iq_bw_"))
    cmd = [
        sys.executable,
        str(ROOT / "tools/bw_test.py"),
        "--udp",
        "--a",
        a,
        "--b",
        b,
        "--host",
        host,
        "--manager",
        str(manager),
        "--conf-a",
        str(conf_a),
        "--conf-b",
        str(conf_b),
        "--modulation",
        "OFDM_24M",
        "--channel",
        "1",
        "--no-cca",
        "--duration",
        "5",
    ]
    if test_ab:
        cmd.append("--test-ab")
    if test_ba:
        cmd.append("--test-ba")
    if not test_ab and not test_ba:
        cmd += ["--test-ab", "--test-ba"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=False)
    text = out.stdout + out.stderr
    if out.returncode != 0:
        raise RuntimeError(f"bw_test failed ({out.returncode}):\n{text[-2000:]}")
    results: list[LegResult] = []
    for label, pat in (
        ("A->B", r"A->B sent \d+ recv \d+\s+([\d.]+) kbps\s+loss ([\d.]+)%"),
        ("B->A", r"B->A sent \d+ recv \d+\s+([\d.]+) kbps\s+loss ([\d.]+)%"),
    ):
        m = re.search(pat, text)
        if m:
            results.append(LegResult(label, float(m.group(1)), float(m.group(2))))
    if not results:
        raise RuntimeError(f"could not parse bw_test output:\n{text[-2000:]}")
    return results


def prepare(a: str, b: str, host: str) -> None:
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/prepare_radios_for_manager.py"),
            "--a",
            a,
            "--b",
            b,
            "--host",
            host,
            "--channel",
            "1",
            "--modulation",
            "OFDM_24M",
            "--no-cca",
        ],
        check=True,
    )
    time.sleep(6)


def main() -> int:
    print(
        "obsolete: lc_tx / set_inject_tune pacing was removed; "
        "use manager winject.tx_burst_* and firmware wifi_tx defaults.",
        file=sys.stderr,
    )
    return 2

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument(
        "--manager",
        default=str(ROOT / "build_manager_arm/winject-manager"),
    )
    p.add_argument(
        "--tune",
        default="max_in_flight=6",
        help="extra set_inject_tune args (both radios)",
    )
    args = p.parse_args()
    manager = Path(args.manager)
    if not manager.is_file():
        print(f"build manager first: {manager}", file=sys.stderr)
        return 1

    modes = (
        ("paced", "queue_mode=paced flush_batch=8 emac_gap_ticks=0 staging_margin=4"),
        ("internal", "queue_mode=internal"),
    )
    gate_mbps = 15.0

    print(f"internal queue compare  A={args.a} B={args.b}  gate={gate_mbps} Mbps/leg\n")
    print(f"{'mode':<10} {'leg':<6} {'Mbps':>8} {'loss%':>7} {'gate':>6}")
    print("-" * 42)

    summary: list[tuple[str, LegResult]] = []
    for mode_name, mode_tune in modes:
        prepare(args.a, args.b, args.host)
        tune = f"{mode_tune} {args.tune}".strip()
        inject_tune(args.a, tune)
        inject_tune(args.b, tune)
        for leg in run_bw(args.a, args.b, args.host, manager, True, True):
            mbps = leg.kbps / 1000.0
            ok = "PASS" if mbps >= gate_mbps else "FAIL"
            print(
                f"{mode_name:<10} {leg.label:<6} {mbps:8.2f} "
                f"{leg.loss_pct:7.1f} {ok:>6}"
            )
            summary.append((mode_name, leg))
        time.sleep(3)

    best = max(summary, key=lambda x: x[1].kbps)
    print(
        f"\nbest cell: {best[0]} {best[1].label} "
        f"{best[1].kbps / 1000:.2f} Mbps loss {best[1].loss_pct:.1f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
