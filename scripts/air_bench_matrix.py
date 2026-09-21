#!/usr/bin/env python3
"""OFDM_24M air matrix: TX→USB (mon0), ESP↔ESP (wifi_bench + radio counters)."""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cons(ip: str, cmd: str, timeout: float = 3.0, need_reply: bool = True) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(cmd.encode(), (ip, 22))
        if not need_reply:
            return ""
        return s.recvfrom(65535)[0].decode(errors="replace")
    except socket.timeout:
        if not need_reply:
            return ""
        raise
    finally:
        s.close()


def wait_bench_idle(ip: str, timeout_s: float = 20.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            wbs = cons(ip, "wifi_bench_status", timeout=5.0)
            if "running=0" in wbs:
                return
        except TimeoutError:
            pass
        time.sleep(0.5)


def gi(text: str, key: str) -> int:
    m = re.search(rf"(?:^|\s){re.escape(key)}=(\d+)", text)
    return int(m.group(1)) if m else 0


def gi_wifi_accept(text: str) -> int:
    if re.search(r"(?:^|\s)wifi_accept=\d", text):
        return gi(text, "wifi_accept")
    return gi(text, "udp_accept")


def bench_stop(ip: str) -> None:
    cons(ip, "wifi_bench_stop", timeout=1.0, need_reply=False)
    wait_bench_idle(ip, timeout_s=20.0)


def prep_radio(ip: str, host_port: int | None) -> None:
    bench_stop(ip)
    for cmd in (
        "set_mode STANDALONE",
        "set_domain 1234",
        "set_channel 1",
        "set_modulation OFDM_24M",
        "set_cca_enabled 0",
        "set_inject_sink wifi",
        "unset_upstream_tx",
        "unset_upstream_rx",
    ):
        cons(ip, cmd)
    if host_port is not None:
        cons(ip, f"set_upstream_rx host=192.168.253.106 port={host_port}")


def run_wifi_bench(ip: str, size: int, count: int, kbps: int) -> dict:
    bench_stop(ip)
    cons(ip, f"set_wifi_bench_defaults size={size} count={count} kbps={kbps}")
    s0 = cons(ip, "status")
    inj0 = gi(s0, "inject_ok")
    # Console reply can be delayed under load; poll status instead.
    cons(
        ip,
        f"wifi_bench_tx size={size} count={count} kbps={kbps}",
        timeout=1.0,
        need_reply=False,
    )
    est_s = (count * size * 8) / (kbps * 1000.0) + 2.0
    time.sleep(est_s)
    wait_bench_idle(ip, timeout_s=est_s + 20.0)
    wbs = cons(ip, "wifi_bench_status", timeout=15.0)
    s1 = cons(ip, "status", timeout=15.0)
    inj1 = gi(s1, "inject_ok")
    el_m = re.search(r"elapsed_us=(-?\d+)", wbs)
    enq_m = re.search(r"enq_ok=(\d+)", wbs)
    elapsed_us = int(el_m.group(1)) if el_m else 0
    enq_ok = int(enq_m.group(1)) if enq_m else 0
    inj_d = inj1 - inj0
    payload = size - 24
    inj_mbps = (
        8.0 * inj_d * payload / elapsed_us * 1e6 / 1e6 if elapsed_us > 0 else 0.0
    )
    return {
        "wbs": wbs,
        "inject_delta": inj_d,
        "enq_ok": enq_ok,
        "elapsed_us": elapsed_us,
        "inject_mbps": inj_mbps,
    }


def parse_sniff_air(text: str) -> tuple[float, int]:
    """Peak interval air= kbps and final bssid frame count."""
    best = 0.0
    frames = 0
    for line in text.splitlines():
        m = re.search(r"air=([\d.]+)\s*kbps", line)
        if m:
            best = max(best, float(m.group(1)))
        m2 = re.search(r"bssid=(\d+)", line)
        if m2:
            frames = int(m2.group(1))
    return best, frames


def bench_usb(radio: str, iface: str, size: int, count: int, kbps: int) -> dict:
    prep_radio(radio, None)
    est_s = max(5.0, (count * size * 8) / (kbps * 1000.0) + 1.0)
    log_path = Path(tempfile.mkdtemp()) / "sniff.log"
    proc = subprocess.Popen(
        [
            "sudo",
            "-n",
            str(ROOT / "scripts/usb_wifi_logger.py"),
            "--iface",
            iface,
            "--channel",
            "1",
            "--bssid",
            "standalone",
            "--duration",
            str(int(est_s) + 3),
            "--interval",
            "2",
            "--quiet",
        ],
        stdout=log_path.open("w"),
        stderr=subprocess.STDOUT,
    )
    time.sleep(1.0)
    bench = run_wifi_bench(radio, size, count, kbps)
    proc.wait(timeout=30)
    sniff_txt = log_path.read_text(errors="replace")
    air_kbps, bssid_n = parse_sniff_air(sniff_txt)
    return {
        **bench,
        "usb_air_kbps": air_kbps,
        "usb_frames": bssid_n,
    }


def bench_peer(tx: str, rx: str, size: int, count: int, kbps: int) -> dict:
    prep_radio(tx, None)
    prep_radio(rx, None)
    cons(rx, "reset_promisc_stats")
    s0_tx = cons(tx, "status")
    s0_rx = cons(rx, "status")
    bench = run_wifi_bench(tx, size, count, kbps)
    s1_tx = cons(tx, "status")
    s1_rx = cons(rx, "status")
    inj_d = gi(s1_tx, "inject_ok") - gi(s0_tx, "inject_ok")
    acc_d = gi_wifi_accept(s1_rx) - gi_wifi_accept(s0_rx)
    elapsed_us = bench["elapsed_us"]
    payload = size - 24
    rx_mbps = (
        8.0 * acc_d * payload / elapsed_us * 1e6 / 1e6 if elapsed_us > 0 else 0.0
    )
    delivery = acc_d / inj_d if inj_d else 0.0
    return {
        **bench,
        "inject_delta": inj_d,
        "peer_accept_delta": acc_d,
        "peer_mbps": rx_mbps,
        "delivery": delivery,
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--iface", default="mon0")
    p.add_argument("--size", type=int, default=1424)
    p.add_argument("--count", type=int, default=15000)
    p.add_argument("--kbps", type=int, default=16514)
    args = p.parse_args()
    ra, rb = args.a, args.b
    for ip in (ra, rb):
        try:
            bench_stop(ip)
        except OSError:
            pass

    rows: list[tuple[str, str, str, str]] = []

    print(f"OFDM_24M ch1  size={args.size} count={args.count} kbps={args.kbps}\n")

    for label, fn in (
        (f"{ra} → USB", lambda: bench_usb(ra, args.iface, args.size, args.count, args.kbps)),
        (f"{rb} → USB", lambda: bench_usb(rb, args.iface, args.size, args.count, args.kbps)),
        (f"{ra} → {rb}", lambda: bench_peer(ra, rb, args.size, args.count, args.kbps)),
        (f"{rb} → {ra}", lambda: bench_peer(rb, ra, args.size, args.count, args.kbps)),
    ):
        print(f"--- {label} ---")
        try:
            r = fn()
        except Exception as e:
            print(f"FAIL: {e}\n")
            rows.append((label, "FAIL", "-", "-"))
            continue
        if "usb_air_kbps" in r:
            print(
                f"inject_ok Δ={r['inject_delta']}  "
                f"inject≈{r['inject_mbps']:.1f} Mbps payload  "
                f"USB air peak={r['usb_air_kbps']:.1f} kbps  "
                f"frames={r['usb_frames']}"
            )
            rows.append(
                (
                    label,
                    f"{r['inject_mbps']:.1f}",
                    f"{r['usb_air_kbps']:.0f}",
                    f"{r['inject_delta']}",
                )
            )
        else:
            print(
                f"inject_ok Δ={r['inject_delta']}  "
                f"peer wifi_accept Δ={r['peer_accept_delta']}  "
                f"delivery={r['delivery']:.1%}  "
                f"peer≈{r['peer_mbps']:.1f} Mbps payload  "
                f"elapsed={r['elapsed_us']/1e6:.2f}s"
            )
            rows.append(
                (
                    label,
                    f"{r['inject_mbps']:.1f}",
                    f"{r['peer_mbps']:.1f}",
                    f"{r['delivery']:.1%}",
                )
            )
        print()

    print("=== summary (Mbps payload / kbps USB air) ===")
    print(f"| {'path':<22} | inject Mbps | USB or peer | notes |")
    print(f"|{'-'*24}|{'-'*13}|{'-'*13}|{'-'*8}|")
    for label, c1, c2, c3 in rows:
        print(f"| {label:<22} | {c1:>11} | {c2:>11} | {c3:>6} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
