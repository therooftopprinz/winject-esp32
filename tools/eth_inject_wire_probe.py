#!/usr/bin/env python3
"""Count host UDP to radio inject port vs radio udp_tx during manager A->B phase."""

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


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument("--iface", default="end1")
    p.add_argument("--inject-port", type=int, default=9000)
    p.add_argument("--kbps", type=float, default=16514)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--size", type=int, default=1400)
    p.add_argument("--send-port", type=int, default=29000)
    p.add_argument("--listen-port", type=int, default=9001)
    args = p.parse_args()

    manager = ROOT / "build_manager_arm" / "winject-manager"
    if not manager.is_file():
        raise SystemExit(f"missing {manager}")

    log_dir = Path(tempfile.mkdtemp(prefix="eth_wire_"))
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
manager.console_in    = 127.0.0.1:2424
manager.console_out   = 127.0.0.1:2425
upstream.size = 2
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 65536
upstream-0.bind_address     = 127.0.0.1:{args.send_port}
upstream-1.mode             = UDP_CLIENT_FORWARDING
upstream-1.tx_bus           = c3
upstream-1.rx_bus           = d4
upstream-1.scheduler_budget = 65536
upstream-1.connect_address  = 127.0.0.1:9001
"""
    )

    for c in (
        "set_mode STANDALONE",
        "set_domain 1234",
        "set_channel 1",
        "set_modulation OFDM_24M",
        "set_tx_power 20",
        "set_cca_enabled 0",
        "set_inject_sink wifi",
        "unset_upstream_tx",
        "unset_upstream_rx",
        f"set_upstream_tx port={args.inject_port}",
        f"set_upstream_rx host={args.host} port=9210",
    ):
        cons(args.a, c)

    mgr = subprocess.Popen(
        [str(manager), str(conf)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)
    if mgr.poll() is not None:
        raise SystemExit("manager failed to start")

    ra0 = RadioSnap.from_status(cons(args.a, "status"))
    gci0 = mgr_gci(("127.0.0.1", 2425), ("127.0.0.1", 2424))
    m = re.search(r"stream tx_pkt=(\d+)", gci0)
    mgr_tx0 = int(m.group(1)) if m else 0

    payload = b"x" * args.size
    wire_n: list[int] = []

    def wire():
        wire_n.append(
            count_tcpdump(args.iface, args.a, args.inject_port, args.duration)
        )

    wt = threading.Thread(target=wire, daemon=True)
    wt.start()
    time.sleep(0.3)

    lis = Listener(args.listen_port)
    sent = pace_send(
        ("127.0.0.1", args.send_port), payload, args.kbps, args.duration
    )
    time.sleep(0.5)
    recv = lis.n
    lis.close()
    wt.join(timeout=10)

    ra1 = RadioSnap.from_status(cons(args.a, "status"))
    gci1 = mgr_gci(("127.0.0.1", 2425), ("127.0.0.1", 2424))
    m = re.search(r"stream tx_pkt=(\d+)", gci1)
    mgr_tx1 = int(m.group(1)) if m else 0
    rd = delta_radio(ra0, ra1)

    mgr.send_signal(subprocess.signal.SIGTERM)
    mgr.wait(timeout=3)

    wire_pkts = wire_n[0] if wire_n else -1
    print(f"host_sent       {sent}")
    print(f"host_recv       {recv}")
    print(f"mgr radio tx    {mgr_tx1 - mgr_tx0}")
    print(f"wire udp :{args.inject_port}  {wire_pkts}")
    print(f"radio udp_tx    {rd['udp_tx']}")
    print(f"radio staging_drop {rd.get('drop_tx_pool', 0)}")
    print(f"radio inject_ok {rd['inject_ok']}")
    if wire_pkts >= 0:
        gap = (mgr_tx1 - mgr_tx0) - rd["udp_tx"]
        print(f"mgr_tx - udp_tx {gap}")
        print(f"wire - udp_tx   {wire_pkts - rd['udp_tx']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
