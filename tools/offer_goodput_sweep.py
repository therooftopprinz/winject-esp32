#!/usr/bin/env python3
"""Sweep host payload offer (kbps) and summarize goodput + drop-stage fractions.

Runs manager_bw_test.sh once per offer (restarts managers each time). Parses the
result table and optional drop-path lines.

Example:
  python3 tools/offer_goodput_sweep.py \\
    --a radio-em0 --b radio-em1 --channel 13 --test-ab \\
    --offers 10000,15000,20000,25000,30000
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def parse_ab_line(text: str) -> tuple[int, int, float, float] | None:
    m = re.search(
        r"A->B sent (\d+) recv (\d+)\s+([\d.]+) kbps\s+loss ([\d.]+)%",
        text,
    )
    if not m:
        return None
    return (
        int(m.group(1)),
        int(m.group(2)),
        float(m.group(3)),
        float(m.group(4)),
    )


def parse_stage_drop(text: str, stage: str) -> tuple[int, int, int] | None:
    pat = rf"^\s*{re.escape(stage)}\s+(\d+) →\s+(\d+)\s+drop\s+(\d+)"
    for line in text.splitlines():
        m = re.match(pat, line)
        if m:
            return int(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="radio-em0")
    p.add_argument("--b", default="radio-em1")
    p.add_argument("--channel", type=int, default=13)
    p.add_argument("--offers", default="10000,15000,20000,25000,30000")
    p.add_argument("--test-ab", action="store_true")
    p.add_argument("--test-ba", action="store_true")
    p.add_argument("--extra", default="", help="extra args to manager_bw_test.sh")
    args = p.parse_args()

    offers = [int(x.strip()) for x in args.offers.split(",") if x.strip()]
    if not offers:
        print("no offers", file=sys.stderr)
        return 2

    script = ROOT / "scripts/manager_bw_test.sh"
    legs = []
    if args.test_ab or (not args.test_ab and not args.test_ba):
        legs.append("--test-ab")
    if args.test_ba:
        legs.append("--test-ba")

    print(
        f"{'offer':>8} {'sent':>6} {'recv':>6} {'deliv%':>7} "
        f"{'Mbps':>8} {'loss%':>7} {'eth_drop%':>9} {'note'}"
    )
    print("-" * 72)

    for kbps in offers:
        cmd = [
            str(script),
            "--a",
            args.a,
            "--b",
            args.b,
            f"--channel",
            str(args.channel),
            "--kbps",
            str(kbps),
            "--drop-stages",
            *legs,
        ]
        if args.extra:
            cmd.extend(args.extra.split())
        proc = subprocess.run(
            cmd,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
        )
        out = proc.stdout + proc.stderr
        if proc.returncode != 0:
            print(f"{kbps:8d}  FAIL rc={proc.returncode}")
            continue
        row = parse_ab_line(out)
        if row is None:
            print(f"{kbps:8d}  FAIL parse A->B line")
            continue
        sent, recv, good_kbps, loss = row
        deliv = 100.0 * recv / sent if sent else 0.0
        eth = parse_stage_drop(out, "mgr_wifi→radio_udp")
        eth_pct = ""
        if eth:
            left, _right, drop = eth
            eth_pct = f"{100.0 * drop / left:.1f}" if left else "n/a"
        note = ""
        if "drain incomplete" in out:
            note = "drain incomplete"
        print(
            f"{kbps:8d} {sent:6d} {recv:6d} {deliv:7.1f} "
            f"{good_kbps / 1000:8.2f} {loss:7.1f} {eth_pct:>9} {note}"
        )

    print(
        "\nInterpretation: if deliv% and eth_drop% stay flat as offer rises, "
        "goodput scales with offer until manager/radio/PHY saturates."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
