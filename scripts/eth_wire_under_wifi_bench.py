#!/usr/bin/env python3
"""wire→udp_tx during idle vs concurrent wifi_bench on the inject radio."""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from drop_path_probe import (  # noqa: E402
    Listener,
    RadioSnap,
    cons,
    delta_radio,
    mgr_gci,
    pace_send,
)


def cons_long(ip: str, cmd: str, timeout: float = 15.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(cmd.encode(), (ip, 22))
        return s.recvfrom(65535)[0].decode(errors="replace")
    finally:
        s.close()


def count_tcpdump(iface: str, host: str, port: int, duration: float) -> int:
    proc = subprocess.Popen(
        [
            "sudo",
            "tcpdump",
            "-i",
            iface,
            "-nn",
            "-q",
            f"host {host} and udp port {port}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    n = 0
    t0 = time.monotonic()
    assert proc.stdout is not None
    for line in proc.stdout:
        if "IP" in line:
            n += 1
        if time.monotonic() - t0 >= duration + 1.0:
            break
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
    return n


def prep_radio_a(ip: str, host: str, inject_port: int = 9000) -> None:
    for c in (
        "wifi_bench_stop",
        "set_mode STANDALONE",
        "set_domain 1234",
        "set_channel 1",
        "set_modulation OFDM_24M",
        "set_cca_enabled 0",
        "set_inject_sink wifi",
        "unset_upstream_tx",
        "unset_upstream_rx",
        f"set_upstream_tx port={inject_port}",
        f"set_upstream_rx host={host} port=9210",
    ):
        cons_long(ip, c)


def start_wifi_bench(ip: str, size: int, count: int, kbps: int) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    try:
        s.sendto(
            f"wifi_bench_tx size={size} count={count} kbps={kbps}".encode(), (ip, 22)
        )
        s.recvfrom(4096)
    except (socket.timeout, OSError):
        pass
    finally:
        s.close()


def bench_running(ip: str) -> bool:
    try:
        wbs = cons_long(ip, "wifi_bench_status", timeout=5.0)
        m = re.search(r"\brunning=(\d+)", wbs)
        return m is not None and m.group(1) == "1"
    except OSError:
        return False


def measure_phase(
    ip: str,
    host: str,
    iface: str,
    manager: Path,
    conf: Path,
    duration: float,
    kbps: float,
    size: int,
    send_port: int,
    status_timeout: float = 15.0,
) -> dict[str, int | float]:
    mgr = subprocess.Popen(
        [str(manager), str(conf)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)
    if mgr.poll() is not None:
        raise RuntimeError("manager failed to start")

    try:
        ra0 = RadioSnap.from_status(cons_long(ip, "status", timeout=status_timeout))
        wire_n: list[int] = []

        def wire():
            wire_n.append(count_tcpdump(iface, ip, 9000, duration))

        payload = b"x" * size
        wt = threading.Thread(target=wire, daemon=True)
        wt.start()
        time.sleep(0.3)
        sent = pace_send(("127.0.0.1", send_port), payload, kbps, duration)
        time.sleep(0.5)
        wt.join(timeout=15)

        ra1 = RadioSnap.from_status(cons_long(ip, "status", timeout=status_timeout))
        rd = delta_radio(ra0, ra1)
    finally:
        mgr.terminate()
        try:
            mgr.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mgr.kill()

    wire_pkts = wire_n[0] if wire_n else -1
    udp = rd["udp_tx"]
    gap = wire_pkts - udp if wire_pkts >= 0 else -1
    pct = 100.0 * gap / wire_pkts if wire_pkts > 0 else 0.0
    return {
        "sent": sent,
        "wire": wire_pkts,
        "udp_tx": udp,
        "gap": gap,
        "gap_pct": pct,
        "inject_ok": rd["inject_ok"],
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument("--iface", default="end1")
    p.add_argument("--kbps", type=float, default=16514)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--size", type=int, default=1400)
    p.add_argument("--bench-kbps", type=int, default=16514)
    p.add_argument("--bench-count", type=int, default=80000)
    p.add_argument("--bench-size", type=int, default=1424)
    args = p.parse_args()

    manager = ROOT / "build_manager_arm" / "winject-manager"
    if not manager.is_file():
        raise SystemExit(f"missing {manager}")

    log_dir = Path(tempfile.mkdtemp(prefix="eth_wbench_"))
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

    print(f"prep {args.a} ...")
    prep_radio_a(args.a, args.host)
    cons_long(args.a, "wifi_bench_stop")
    time.sleep(0.5)

    print("\n=== idle (no wifi_bench) ===")
    idle = measure_phase(
        args.a,
        args.host,
        args.iface,
        manager,
        conf,
        args.duration,
        args.kbps,
        args.size,
        29000,
    )
    print(idle)

    print("\n=== concurrent wifi_bench + manager ETH inject ===")
    start_wifi_bench(args.a, args.bench_size, args.bench_count, args.bench_kbps)
    time.sleep(1.0)
    running = bench_running(args.a)
    print(f"wifi_bench running={running}")
    cont = measure_phase(
        args.a,
        args.host,
        args.iface,
        manager,
        conf,
        args.duration,
        args.kbps,
        args.size,
        29000,
    )
    print(cont)
    try:
        cons_long(args.a, "wifi_bench_stop", timeout=5.0)
    except OSError:
        pass

    print("\n=== summary ===")
    print(
        f"idle:  wire={idle['wire']} udp_tx={idle['udp_tx']} "
        f"gap={idle['gap']} ({idle['gap_pct']:.1f}%)"
    )
    print(
        f"cont:  wire={cont['wire']} udp_tx={cont['udp_tx']} "
        f"gap={cont['gap']} ({cont['gap_pct']:.1f}%)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
