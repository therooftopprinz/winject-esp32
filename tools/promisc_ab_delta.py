#!/usr/bin/env python3
"""Reset promisc stats, run one A→B manager bw_test, print .14 RX/promisc deltas."""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cons(ip: str, cmd: str, timeout: float = 6.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(cmd.encode(), (ip, 22))
    chunks: list[bytes] = []
    while True:
        try:
            d, _ = s.recvfrom(65535)
            chunks.append(d)
            if len(d) < 8000:
                break
        except socket.timeout:
            break
    s.close()
    return b"".join(chunks).decode(errors="replace")


def grab_int(text: str, key: str) -> int:
    m = re.search(rf"(?:^|\\s){re.escape(key)}=([0-9]+)", text)
    return int(m.group(1)) if m else 0


def grab_wifi_accept_line(rx_line: str) -> int:
    if re.search(r"(?:^|\\s)wifi_accept=\d", rx_line):
        return grab_int(rx_line, "wifi_accept")
    return grab_int(rx_line, "udp_accept")


def snap_promisc(text: str) -> dict[str, int]:
    m = re.search(
        r"promisc data=(\d+) misc=\d+ misc_len=\d+ ctrl=\d+ skip_type=\d+ "
        r"sig_ht=(\d+) sig_legacy=(\d+) sig_other=\d+ drop_ampdu=\d+ "
        r"drop_len=(\d+) drop_addr3=(\d+).*?leg_ok=(\d+) domain_word=(\d+)",
        text,
    )
    if not m:
        return {}
    keys = [
        "data",
        "sig_ht",
        "sig_legacy",
        "drop_len",
        "drop_addr3",
        "leg_ok",
        "domain_word",
    ]
    return {k: int(m.group(i + 1)) for i, k in enumerate(keys)}


def snap_rx(text: str) -> dict[str, int]:
    rx = next((ln for ln in text.splitlines() if ln.startswith("channel_rx")), "")
    return {
        "wifi_accept": grab_wifi_accept_line(rx),
        "udp_fwd": grab_int(rx, "udp_fwd"),
        "drop_crc_error": grab_int(rx, "drop_crc_error"),
        "drop_rx_pool": grab_int(rx, "drop_no_pkt_pool"),
        "drop_rx_q": grab_int(rx, "drop_queue_full"),
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--host", default="192.168.253.106")
    args = p.parse_args()

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/prepare_radios_for_manager.py"),
            "--a",
            args.a,
            "--b",
            args.b,
            "--host",
            args.host,
            "--channel",
            "1",
            "--modulation",
            "OFDM_24M",
            "--no-cca",
        ],
        check=True,
    )
    time.sleep(8)
    cons(args.b, "reset_promisc_stats\n")
    time.sleep(0.2)
    before = cons(args.b, "status\n")
    pb = snap_promisc(before)
    rb = snap_rx(before)

    r = subprocess.run(
        [
            "bash",
            str(ROOT / "scripts/manager_bw_test.sh"),
            "--a",
            args.a,
            "--b",
            args.b,
            "--no-cca",
            "--channel",
            "1",
            "--test-ab",
        ],
        capture_output=True,
        text=True,
    )
    print(r.stdout[-1200:] if r.stdout else r.stderr)
    after = cons(args.b, "status\n")
    pa = snap_promisc(after)
    ra = snap_rx(after)

    print("\n=== .14 promisc delta (one A→B run) ===")
    for k in sorted(pb):
        print(f"  {k:14s}  {pa.get(k, 0) - pb.get(k, 0):8d}")
    print("\n=== .14 channel_rx delta ===")
    for k in sorted(rb):
        print(f"  {k:18s}  {ra.get(k, 0) - rb.get(k, 0):8d}")

    sent = recv = 0
    m = re.search(r"A->B sent (\d+) recv (\d+)", r.stdout or "")
    if m:
        sent, recv = int(m.group(1)), int(m.group(2))
        print(f"\nhost A→B: sent {sent} recv {recv} loss {(sent-recv)*100/sent:.1f}%")
        air_ok = pa.get("leg_ok", 0) - pb.get("leg_ok", 0)
        print(f".14 promisc leg_ok delta {air_ok}  (vs host recv {recv})")
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
