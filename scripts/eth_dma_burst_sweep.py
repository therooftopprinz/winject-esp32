#!/usr/bin/env python3
"""Sweep EMAC dma_burst_len on radio A; measure wire→udp_tx under manager inject."""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from drop_path_probe import cons  # noqa: E402


def wait_radio(ip: str, timeout_s: float = 60.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            cons(ip, "status", timeout=2.0)
            return True
        except OSError:
            time.sleep(2.0)
    return False


def burst_from_status(ip: str) -> int | None:
    text = cons(ip, "status", timeout=3.0)
    m = re.search(r"eth_dma_burst=(\d+)", text)
    return int(m.group(1)) if m else None


def run_wire_probe() -> dict[str, int]:
    proc = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "eth_inject_wire_probe.py"),
            "--duration",
            "5",
        ],
        capture_output=True,
        text=True,
        cwd=str(ROOT),
        check=False,
    )
    out = proc.stdout + proc.stderr
    vals: dict[str, int] = {}
    patterns = (
        (r"^wire udp :9000\s+(\d+)", "wire"),
        (r"^radio udp_tx\s+(\d+)", "udp_tx"),
        (r"^wire - udp_tx\s+(-?\d+)", "gap"),
    )
    for pat, name in patterns:
        m = re.search(pat, out, re.M)
        if m:
            vals[name] = int(m.group(1))
    if proc.returncode != 0 and "wire" not in vals:
        raise RuntimeError(f"eth_inject_wire_probe failed:\n{out[-800:]}")
    return vals


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument(
        "--bursts",
        default="32,16,8,4",
        help="comma-separated beats (applied on .9 only; reboot each)",
    )
    args = p.parse_args()
    bursts = [int(x.strip()) for x in args.bursts.split(",") if x.strip()]

    print("EMAC dma_burst_len sweep (manager eth_inject_wire_probe, 5s each)\n")
    print(f"| burst | wire :9000 | udp_tx | gap (wire-udp_tx) |")
    print(f"|-------|------------|--------|-------------------|")

    for beats in bursts:
        cmd = f"set_eth_dma_burst_len {beats}"
        try:
            reply = cons(args.a, cmd, timeout=5.0)
        except OSError as e:
            print(f"| {beats:5d} | FAIL set: {e}")
            continue
        if not reply.strip().startswith("ok"):
            print(f"| {beats:5d} | FAIL console: {reply.strip()[:60]}")
            continue
        if not wait_radio(args.a):
            print(f"| {beats:5d} | FAIL timeout after reboot")
            continue
        got = burst_from_status(args.a)
        if got != beats:
            print(f"| {beats:5d} | WARN status eth_dma_burst={got}")
        time.sleep(3.0)
        try:
            m = run_wire_probe()
        except RuntimeError as e:
            print(f"| {beats:5d} | FAIL probe: {e}")
            continue
        wire = m.get("wire", -1)
        udp = m.get("udp_tx", -1)
        gap = m.get("gap", wire - udp if wire >= 0 and udp >= 0 else -1)
        print(f"| {beats:5d} | {wire:10d} | {udp:6d} | {gap:17d} |")
        time.sleep(2.0)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
