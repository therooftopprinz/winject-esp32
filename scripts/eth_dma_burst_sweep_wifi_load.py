#!/usr/bin/env python3
"""Sweep EMAC dma_burst_len while wifi_bench runs on the inject radio."""

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
sys.path.insert(0, str(ROOT / "tools"))

from drop_path_probe import cons  # noqa: E402

# Reuse phase measurement from eth_wire_under_wifi_bench
sys.path.insert(0, str(ROOT / "scripts"))
from eth_wire_under_wifi_bench import (  # noqa: E402
    bench_running,
    measure_phase,
    prep_radio_a,
    start_wifi_bench,
)


def wait_radio(ip: str, timeout_s: float = 90.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            cons(ip, "status", timeout=3.0)
            return True
        except OSError:
            time.sleep(2.0)
    return False


def burst_from_status(ip: str) -> int | None:
    text = cons(ip, "status", timeout=8.0)
    m = re.search(r"eth_dma_burst=(\d+)", text)
    return int(m.group(1)) if m else None


def cons_long(ip: str, cmd: str, timeout: float = 15.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(cmd.encode(), (ip, 22))
        return s.recvfrom(65535)[0].decode(errors="replace")
    finally:
        s.close()


def kill_stale_managers() -> None:
    subprocess.run(
        ["pkill", "-f", "winject-manager"],
        capture_output=True,
        check=False,
    )
    time.sleep(0.5)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument("--iface", default="end1")
    p.add_argument("--bursts", default="32,16,8,4,2,1")
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--kbps", type=float, default=16514)
    p.add_argument("--bench-kbps", type=int, default=16514)
    p.add_argument("--bench-count", type=int, default=80000)
    p.add_argument("--bench-size", type=int, default=1424)
    p.add_argument("--idle-baseline", action="store_true", help="also run idle phase per burst")
    p.add_argument(
        "--no-restore",
        action="store_true",
        help="leave last burst in NVS (default: restore dma_burst_len=32)",
    )
    args = p.parse_args()
    bursts = [int(x.strip()) for x in args.bursts.split(",") if x.strip()]

    manager = ROOT / "build_manager_arm" / "winject-manager"
    if not manager.is_file():
        raise SystemExit(f"missing {manager}")

    log_dir = Path(tempfile.mkdtemp(prefix="dma_burst_wbench_"))
    conf = log_dir / "mgr_a.cfg"
    conf.write_text(
        f"""
winject.device        = {args.a}
winject.local_ip      = {args.host}
winject.console       = 22
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
winject.domain        = 1234
winject.max_rate_kbps = {int(args.kbps)}
winject.stats_sec     = 1
winject.ci_pace_inject = 1
winject.skip_console  = 1
winject.forward_base  = 9210
upstream.size = 2
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 65536
upstream-0.bind_address     = 127.0.0.1:29000
upstream-1.mode             = UDP_CLIENT_FORWARDING
upstream-1.tx_bus           = c3
upstream-1.rx_bus           = d4
upstream-1.scheduler_budget = 65536
upstream-1.connect_address  = 127.0.0.1:9001
"""
    )

    kill_stale_managers()

    def log(line: str) -> None:
        print(line, flush=True)

    log("dma_burst_len sweep under wifi_bench + manager ETH inject\n")
    hdr = "| burst | wbench | wire | udp_tx | gap | gap% |"
    sep = "|-------|--------|------|--------|-----|------|"

    log(hdr)
    log(sep)

    for beats in bursts:
        try:
            cons_long(args.a, "wifi_bench_stop", timeout=5.0)
        except OSError:
            pass
        try:
            reply = cons_long(args.a, f"set_eth_dma_burst_len {beats}", timeout=8.0)
        except (TimeoutError, OSError) as e:
            log(f"| {beats:5d} | FAIL set timeout: {e} |")
            wait_radio(args.a, timeout_s=30.0)
            continue
        if not reply.strip().startswith("ok"):
            log(f"| {beats:5d} | FAIL set: {reply.strip()[:50]} |")
            continue
        if not wait_radio(args.a):
            log(f"| {beats:5d} | FAIL reboot timeout |")
            continue
        got = burst_from_status(args.a)
        prep_radio_a(args.a, args.host)
        time.sleep(1.0)

        kill_stale_managers()
        if args.idle_baseline:
            try:
                cons_long(args.a, "wifi_bench_stop")
                time.sleep(0.5)
                idle = measure_phase(
                    args.a,
                    args.host,
                    args.iface,
                    manager,
                    conf,
                    args.duration,
                    args.kbps,
                    1400,
                    29000,
                )
                log(
                    f"| {beats:5d} | idle   | {idle['wire']:4d} | "
                    f"{idle['udp_tx']:6d} | {idle['gap']:3d} | {idle['gap_pct']:5.1f} |"
                )
            except Exception as e:
                log(f"| {beats:5d} | idle   | ERR {e} |")

        kill_stale_managers()
        start_wifi_bench(
            args.a, args.bench_size, args.bench_count, args.bench_kbps
        )
        time.sleep(2.0)
        running = bench_running(args.a)
        if not running:
            start_wifi_bench(
                args.a, args.bench_size, args.bench_count, args.bench_kbps
            )
            time.sleep(2.0)
            running = bench_running(args.a)
        try:
            cont = measure_phase(
                args.a,
                args.host,
                args.iface,
                manager,
                conf,
                args.duration,
                args.kbps,
                1400,
                29000,
                status_timeout=45.0,
            )
        except Exception as e:
            log(f"| {beats:5d} | {str(running):6s} | ERR {e} |")
            try:
                cons_long(args.a, "wifi_bench_stop", timeout=5.0)
            except OSError:
                pass
            continue
        try:
            cons_long(args.a, "wifi_bench_stop", timeout=8.0)
        except OSError:
            pass
        log(
            f"| {beats:5d} | {str(running):6s} | {cont['wire']:4d} | "
            f"{cont['udp_tx']:6d} | {cont['gap']:3d} | {cont['gap_pct']:5.1f} |"
        )
        time.sleep(3.0)

    if not args.no_restore:
        try:
            cons_long(args.a, "wifi_bench_stop", timeout=5.0)
        except OSError:
            pass
        log("\nrestoring eth_dma_burst_len=32 (reboot) ...")
        try:
            reply = cons_long(args.a, "set_eth_dma_burst_len 32", timeout=8.0)
        except (TimeoutError, OSError) as e:
            log(f"restore failed: {e}")
            return 1
        if not reply.strip().startswith("ok"):
            log(f"restore failed: {reply.strip()[:60]}")
            return 1
        if wait_radio(args.a):
            prep_radio_a(args.a, args.host)
            got = burst_from_status(args.a)
            log(f"restore ok eth_dma_burst={got}")
        else:
            log("restore: reboot timeout")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
