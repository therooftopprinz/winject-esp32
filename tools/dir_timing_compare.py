#!/usr/bin/env python3
"""Compare A→B vs B→A: host goodput/loss, inject timing, recv promisc deltas.

Runs manager_bw_test one leg at a time (prepare once), resets promisc on the
listener before each leg, then prints inject ``inject_wait`` / pool / queue
from ``status`` and promisc deltas on the peer.

Example:
  python3 tools/dir_timing_compare.py --a 192.168.253.9 --b 192.168.253.14
  python3 tools/dir_timing_compare.py --a 192.168.253.14 --b 192.168.253.9  # swap roles
"""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
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


def grab_us(text: str, key: str) -> int | None:
    m = re.search(rf"{re.escape(key)}=(\d+)us", text)
    return int(m.group(1)) if m else None


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
    }


@dataclass
class InjectSnap:
    tx_latency_us: int | None
    inject_wait_us: int | None
    pool_free_min: int
    queue_hwm: int
    inject_ok: int
    udp_tx: int

    @classmethod
    def from_status(cls, text: str) -> InjectSnap:
        tx = next((ln for ln in text.splitlines() if ln.startswith("channel_tx")), "")
        return cls(
            tx_latency_us=grab_us(tx, "tx_latency"),
            inject_wait_us=grab_us(tx, "inject_wait"),
            pool_free_min=grab_int(tx, "pool_free_min"),
            queue_hwm=grab_int(tx, "queue_hwm"),
            inject_ok=grab_int(tx, "inject_ok"),
            udp_tx=grab_int(tx, "udp_tx"),
        )


@dataclass
class LegResult:
    label: str
    inject_ip: str
    recv_ip: str
    host_kbps: float
    host_loss_pct: float
    host_sent: int
    host_recv: int
    inject: InjectSnap
    promisc_delta: dict[str, int]
    rx_delta: dict[str, int]


def run_bw_leg(
    radio_a: str,
    radio_b: str,
    host: str,
    channel: int,
    modulation: str,
    test_flag: str,
) -> tuple[str, int]:
    cmd = [
        "bash",
        str(ROOT / "scripts/manager_bw_test.sh"),
        "--a",
        radio_a,
        "--b",
        radio_b,
        "--host",
        host,
        "--channel",
        str(channel),
        "--modulation",
        modulation,
        "--no-cca",
        test_flag,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    return out, r.returncode


def parse_host_line(out: str, pattern: str) -> tuple[float, float, int, int]:
    m = re.search(
        pattern + r" sent (\d+) recv (\d+)\s+([\d.]+) kbps\s+loss ([\d.]+)%",
        out,
    )
    if not m:
        return 0.0, 0.0, 0, 0
    sent, recv = int(m.group(1)), int(m.group(2))
    kbps = float(m.group(3))
    loss = float(m.group(4))
    return kbps, loss, sent, recv


def run_leg(
    label: str,
    inject_ip: str,
    recv_ip: str,
    radio_a: str,
    radio_b: str,
    host: str,
    channel: int,
    modulation: str,
    test_flag: str,
    host_pat: str,
) -> LegResult:
    cons(recv_ip, "reset_promisc_stats\n")
    time.sleep(0.15)
    before = cons(recv_ip, "status\n")
    pb = snap_promisc(before)
    rb = snap_rx(before)

    out, _rc = run_bw_leg(radio_a, radio_b, host, channel, modulation, test_flag)
    for ln in out.splitlines():
        if "sent" in ln and "kbps" in ln and "loss" in ln:
            print(f"  {ln.strip()}")

    after_recv = cons(recv_ip, "status\n")
    pa = snap_promisc(after_recv)
    ra = snap_rx(after_recv)
    inject = InjectSnap.from_status(cons(inject_ip, "status\n"))

    kbps, loss, sent, recv = parse_host_line(out, host_pat)
    prom_d = {k: pa.get(k, 0) - pb.get(k, 0) for k in pb}
    rx_d = {k: ra.get(k, 0) - rb.get(k, 0) for k in rb}

    return LegResult(
        label=label,
        inject_ip=inject_ip,
        recv_ip=recv_ip,
        host_kbps=kbps,
        host_loss_pct=loss,
        host_sent=sent,
        host_recv=recv,
        inject=inject,
        promisc_delta=prom_d,
        rx_delta=rx_d,
    )


def print_leg(r: LegResult) -> None:
    print(f"\n--- {r.label}: inject {r.inject_ip} → recv {r.recv_ip} ---")
    print(
        f"  host  {r.host_kbps:.1f} kbps  loss {r.host_loss_pct:.1f}%  "
        f"({r.host_recv}/{r.host_sent})"
    )
    iw = r.inject.inject_wait_us
    tl = r.inject.tx_latency_us
    print(
        f"  inject timing  tx_latency={tl if tl is not None else '-'}us  "
        f"inject_wait={iw if iw is not None else '-'}us  "
        f"pool_free_min={r.inject.pool_free_min}  queue_hwm={r.inject.queue_hwm}"
    )
    leg = r.promisc_delta.get("leg_ok", 0)
    d3 = r.promisc_delta.get("drop_addr3", 0)
    data = r.promisc_delta.get("data", 0)
    gap = leg - r.host_recv if leg else 0
    addr3_pct = (100.0 * d3 / data) if data else 0.0
    print(
        f"  recv promisc Δ  leg_ok={leg}  drop_addr3={d3} ({addr3_pct:.1f}% of data)  "
        f"promisc→host gap={gap}"
    )
    crc = r.rx_delta.get("drop_crc_error", 0)
    udp = r.rx_delta.get("wifi_accept", 0)
    if udp or crc:
        print(f"  recv channel_rx Δ  wifi_accept={udp}  drop_crc_error={crc}")


def print_summary(rows: list[LegResult]) -> None:
    print("\n=== summary ===")
    print(
        f"{'leg':<8} {'inject':<16} {'recv':<16} "
        f"{'kbps':>8} {'loss%':>6} {'inj_wait':>8} {'pool_min':>8} "
        f"{'q_hwm':>5} {'leg_okΔ':>7} {'addr3Δ':>7}"
    )
    for r in rows:
        iw = r.inject.inject_wait_us
        print(
            f"{r.label:<8} {r.inject_ip:<16} {r.recv_ip:<16} "
            f"{r.host_kbps:8.1f} {r.host_loss_pct:6.1f} "
            f"{iw if iw is not None else 0:8d} "
            f"{r.inject.pool_free_min:8d} {r.inject.queue_hwm:5d} "
            f"{r.promisc_delta.get('leg_ok', 0):7d} "
            f"{r.promisc_delta.get('drop_addr3', 0):7d}"
        )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument("--channel", type=int, default=1)
    p.add_argument("--modulation", default="OFDM_24M")
    p.add_argument(
        "--prepare",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="run prepare_radios_for_manager once before legs",
    )
    p.add_argument("--ab-only", action="store_true", help="only A→B leg")
    p.add_argument("--ba-only", action="store_true", help="only B→A leg")
    args = p.parse_args()

    if args.prepare:
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
                str(args.channel),
                "--modulation",
                args.modulation,
                "--no-cca",
            ],
            check=True,
        )
        time.sleep(8)

    legs: list[tuple[str, str, str, str, str]] = []
    if not args.ba_only:
        legs.append(
            ("A→B", args.a, args.b, "--test-ab", r"A->B"),
        )
    if not args.ab_only:
        legs.append(
            ("B→A", args.b, args.a, "--test-ba", r"B->A"),
        )

    print(
        f"radios A={args.a} B={args.b} host={args.host} "
        f"ch={args.channel} {args.modulation}"
    )
    rows: list[LegResult] = []
    rc = 0
    for label, inject_ip, recv_ip, flag, host_pat in legs:
        print(f"\n{'='*60}\n{label}\n{'='*60}")
        r = run_leg(
            label,
            inject_ip,
            recv_ip,
            args.a,
            args.b,
            args.host,
            args.channel,
            args.modulation,
            flag,
            host_pat,
        )
        print_leg(r)
        rows.append(r)
        if r.host_sent == 0:
            rc = 1

    if len(rows) > 1:
        print_summary(rows)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
