#!/usr/bin/env python3
"""Run manager B→A bw_test while sniffing mon0; compare air vs host duplicates."""

from __future__ import annotations

import argparse
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import bw_test  # noqa: E402
import mpdu  # noqa: E402

from usb_wifi_logger import (  # noqa: E402
    PREFIX_STANDALONE,
    addr3_matches,
    open_monitor,
    parse_dot11,
    parse_radiotap,
    resolve_iface,
    set_channel,
)

parse_seq = bw_test.parse_seq

# B→A on bench: manager B upstream-1 tx_bus d4 (.14 → air); A rx d4.
BUS_BA = mpdu.parse_bus_int("d4")
AIR_SEQ_LEN = 2
DOMAIN = mpdu.parse_domain_int("1234")


def strip_air_seq(body: bytes) -> tuple[int | None, bytes]:
    if len(body) < AIR_SEQ_LEN:
        return None, b""
    air = (body[0] << 8) | body[1]
    return air, body[AIR_SEQ_LEN:]


@dataclass
class AirStats:
    frames: int = 0
    bodies: int = 0
    bodies_ba: int = 0
    dup_bodies: int = 0
    seqs: set[int] = field(default_factory=set)
    tag_b_seqs: set[int] = field(default_factory=set)
    tag_b_dup: int = 0
    air_seqs: set[int] = field(default_factory=set)
    air_seq_dup: int = 0
    bus_hist: Counter[int] = field(default_factory=Counter)
    wifi_seq_dups: int = 0
    wifi_seqs: set[int] = field(default_factory=set)
    retry_frames: int = 0


class AirSniffer:
    def __init__(self, iface: str) -> None:
        self.iface = iface
        self.stats = AirStats()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=5.0)

    def _run(self) -> None:
        sock = open_monitor(self.iface)
        filters = {PREFIX_STANDALONE}
        try:
            while not self._stop.is_set():
                try:
                    pkt = sock.recv(65535)
                except socket.timeout:
                    continue
                rt = parse_radiotap(pkt)
                if rt is None:
                    continue
                raw = pkt[rt.length :]
                dot = parse_dot11(raw, rt.fcs)
                if dot is None or not dot.is_data:
                    continue
                if not addr3_matches(dot.bssid, filters):
                    continue
                got_domain = (dot.bssid[4] << 8) | dot.bssid[5]
                if got_domain != DOMAIN:
                    continue
                st = self.stats
                st.frames += 1
                if dot.retry:
                    st.retry_frames += 1
                if dot.seq in st.wifi_seqs:
                    st.wifi_seq_dups += 1
                st.wifi_seqs.add(dot.seq)
                mpdu_bytes = raw[: dot.hdr_len] + dot.body
                if len(mpdu_bytes) < mpdu.WIFI_HDR_LEN:
                    continue
                for bus, body in mpdu.unpack_mpdu(mpdu_bytes):
                    st.bodies += 1
                    st.bus_hist[bus] += 1
                    if bus != BUS_BA:
                        continue
                    st.bodies_ba += 1
                    air_n, payload = strip_air_seq(body)
                    if air_n is not None:
                        if air_n in st.air_seqs:
                            st.air_seq_dup += 1
                        else:
                            st.air_seqs.add(air_n)
                    seq = parse_seq(payload)
                    if seq is None:
                        continue
                    if seq in st.seqs:
                        st.dup_bodies += 1
                    else:
                        st.seqs.add(seq)
                    if payload.startswith(b"B "):
                        if seq in st.tag_b_seqs:
                            st.tag_b_dup += 1
                        else:
                            st.tag_b_seqs.add(seq)
        finally:
            sock.close()


def cons(ip: str, cmd: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5.0)
    s.sendto((cmd + "\n").encode(), (ip, 22))
    return s.recvfrom(65535)[0].decode(errors="replace")


def gi(text: str, key: str) -> int:
    m = re.search(rf"(?:^|\s){re.escape(key)}=(\d+)", text)
    return int(m.group(1)) if m else 0


def gi_wifi_accept(text: str) -> int:
    if re.search(r"(?:^|\s)wifi_accept=\d", text):
        return gi(text, "wifi_accept")
    return gi(text, "udp_accept")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--iface", default=None)
    p.add_argument("--channel", type=int, default=1)
    p.add_argument("--kbps", type=int, default=15000)
    p.add_argument("--warmup", type=float, default=1.0)
    args = p.parse_args()

    iface = resolve_iface(args.iface)
    set_channel(iface, args.channel)

    for ip in (args.a, args.b):
        cons(ip, "reset_promisc_stats")

    sniff = AirSniffer(iface)
    sniff.start()
    time.sleep(args.warmup)

    st_a0 = cons(args.a, "status")
    st_b0 = cons(args.b, "status")
    acc_a0 = gi_wifi_accept(st_a0)
    inj_b0 = gi(st_b0, "inject_ok")

    cmd = [
        str(ROOT / "scripts/manager_bw_test.sh"),
        "--a",
        args.a,
        "--b",
        args.b,
        "--no-cca",
        "--channel",
        str(args.channel),
        "--test-ba",
        "--kbps",
        str(args.kbps),
    ]
    print("running:", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    print(out, end="" if out.endswith("\n") else "\n")

    time.sleep(0.5)
    sniff.stop()

    st_a1 = cons(args.a, "status")
    st_b1 = cons(args.b, "status")
    acc_a1 = gi_wifi_accept(st_a1)
    inj_b1 = gi(st_b1, "inject_ok")

    host_sent = host_recv = host_dup = None
    m = re.search(
        r"B->A sent (\d+) recv (\d+)\s+[\d.]+\s+kbps\s+loss [\d.]+%(?:\s+dup_ignored (\d+))?",
        out,
    )
    if m:
        host_sent = int(m.group(1))
        host_recv = int(m.group(2))
        host_dup = int(m.group(3) or 0)

    air = sniff.stats
    print("\n=== duplicate probe summary ===")
    print(f"USB iface {iface}  domain 0x{DOMAIN:04x}  bus_ba 0x{BUS_BA:02x}")
    top_bus = ", ".join(f"0x{b:02x}:{n}" for b, n in air.bus_hist.most_common(6))
    print(
        f"air:  frames={air.frames}  retry_frames={air.retry_frames}  "
        f"802.11_seq_dups={air.wifi_seq_dups}  "
        f"mpdu_bodies={air.bodies}  ba_bodies={air.bodies_ba}  "
        f"ba_unique_app_seq={len(air.seqs)}  ba_dup_app_seq={air.dup_bodies}  "
        f"ba_unique_air_seq={len(air.air_seqs)}  ba_dup_air_seq={air.air_seq_dup}"
    )
    print(f"air bus histogram (top): {top_bus or 'none'}")
    print(
        f"radio .9 wifi_accept Δ={acc_a1 - acc_a0}  "
        f".14 inject_ok Δ={inj_b1 - inj_b0}"
    )
    if host_sent is not None:
        print(
            f"host: sent={host_sent}  unique_recv={host_recv}  "
            f"dup_ignored={host_dup}  "
            f"host_extra_vs_unique={host_dup}"
        )
        if air.bodies_ba > 0:
            ratio = air.dup_bodies / max(1, air.bodies_ba)
            print(
                f"interpretation: air duplicate app-seq rate "
                f"{100.0 * ratio:.1f}% of ba MPDU bodies"
            )
        air_app_dup = air.dup_bodies
        if host_dup and air_app_dup == 0 and air.wifi_seq_dups == 0:
            print(
                "interpretation: host dup_ignored>0 but USB shows no air dup "
                "→ duplicates after WiFi (radio UDP forward / manager demux on :9001)"
            )
        elif host_dup and air_app_dup > 0:
            print(
                "interpretation: duplicate app payloads seen on USB and at host "
                "→ on-air replays and/or stack duplicates both contribute"
            )
        elif host_dup and air_app_dup == 0:
            print(
                "interpretation: host dup_ignored>0, USB shows unique app-seq per "
                "B→A MPDU → duplicates arise after WiFi (radio→manager→:9001), "
                "not as repeated identical payloads on mon0"
            )
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
