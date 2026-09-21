#!/usr/bin/env python3
"""B→A duplicate attribution: radio counters vs Ethernet forward (9210) vs host (9001)."""

from __future__ import annotations

import argparse
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bw_test  # noqa: E402
import mpdu  # noqa: E402

parse_seq = bw_test.parse_seq
BUS_BA = mpdu.parse_bus_int("d4")
AIR_SEQ_LEN = 2
FWD_PORT = 9210
HOST_PORT = 9001


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


@dataclass
class SeqStats:
    total: int = 0
    seqs: set[int] = field(default_factory=set)
    dup: int = 0

    def note(self, payload: bytes) -> None:
        self.total += 1
        seq = parse_seq(payload)
        if seq is None:
            return
        if seq in self.seqs:
            self.dup += 1
        else:
            self.seqs.add(seq)


def strip_air(payload: bytes) -> bytes:
    if len(payload) < AIR_SEQ_LEN:
        return payload
    return payload[AIR_SEQ_LEN:]


def note_mpdu(stats: SeqStats, data: bytes) -> None:
    if len(data) < mpdu.WIFI_HDR_LEN:
        return
    for bus, body in mpdu.unpack_mpdu(data):
        if bus != BUS_BA:
            continue
        stats.note(strip_air(body))


def analyze_pcap(path: Path) -> tuple[SeqStats, SeqStats, Counter[int]]:
    fwd = SeqStats()
    host = SeqStats()
    ports: Counter[int] = Counter()
    with path.open("rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return fwd, host, ports
        magic = struct.unpack("<I", gh[:4])[0]
        endian = "<" if magic == 0xA1B2C3D4 else ">"
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            _, _, incl, _ = struct.unpack(endian + "IIII", h)
            frame = f.read(incl)
            if len(frame) < 14:
                continue
            eth_type = struct.unpack("!H", frame[12:14])[0]
            if eth_type != 0x0800:
                continue
            ip = frame[14:]
            if len(ip) < 20:
                continue
            ihl = (ip[0] & 0x0F) * 4
            if ip[9] != 17:
                continue
            udp_off = 14 + ihl
            if len(frame) < udp_off + 8:
                continue
            dport = struct.unpack("!H", frame[udp_off + 2 : udp_off + 4])[0]
            ulen = struct.unpack("!H", frame[udp_off + 4 : udp_off + 6])[0]
            payload_off = udp_off + 8
            payload = frame[payload_off : payload_off + max(0, ulen - 8)]
            ports[dport] += 1
            if dport == FWD_PORT:
                note_mpdu(fwd, payload)
            elif dport == HOST_PORT:
                host.note(payload)
    return fwd, host, ports


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="radio-em0")
    p.add_argument("--b", default="radio-em1")
    p.add_argument("--kbps", type=int, default=15000)
    args = p.parse_args()

    for ip in (args.a, args.b):
        cons(ip, "reset_promisc_stats")

    st0 = cons(args.a, "status")
    acc0 = gi_wifi_accept(st0)
    fwd0 = gi(st0, "udp_fwd")
    pd0 = gi(st0, "promisc_data")
    pm0 = gi(st0, "promisc_misc")

    pcap = Path(tempfile.mkdtemp(prefix="dup_eth_")) / "dup.pcap"
    pcap_lo = pcap.parent / "host_lo.pcap"
    caps = [
        subprocess.Popen(
            [
                "sudo",
                "tcpdump",
                "-i",
                "end1",
                "-n",
                "-s",
                "2048",
                "-w",
                str(pcap),
                f"udp port {FWD_PORT}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ),
        subprocess.Popen(
            [
                "sudo",
                "tcpdump",
                "-i",
                "lo",
                "-n",
                "-s",
                "2048",
                "-w",
                str(pcap_lo),
                f"udp port {HOST_PORT}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ),
    ]
    time.sleep(0.3)

    cmd = [
        str(ROOT / "scripts/manager_bw_test.sh"),
        "--a",
        args.a,
        "--b",
        args.b,
        "--no-cca",
        "--channel",
        "1",
        "--test-ba",
        "--kbps",
        str(args.kbps),
    ]
    print("running:", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    print(out, end="" if out.endswith("\n") else "\n")

    time.sleep(0.5)
    for cap in caps:
        cap.send_signal(signal.SIGINT)
    for cap in caps:
        cap.wait(timeout=10)

    st1 = cons(args.a, "status")
    acc1 = gi_wifi_accept(st1)
    fwd1 = gi(st1, "udp_fwd")
    pd1 = gi(st1, "promisc_data")
    pm1 = gi(st1, "promisc_misc")

    eth_fwd, _, port_hist = analyze_pcap(pcap)
    eth_host, _, port_lo = analyze_pcap(pcap_lo)
    port_hist.update(port_lo)

    host_sent = host_recv = host_dup = None
    m = re.search(
        r"B->A sent (\d+) recv (\d+)\s+[\d.]+\s+kbps\s+loss [\d.]+%(?:\s+dup_ignored (\d+))?",
        out,
    )
    if m:
        host_sent = int(m.group(1))
        host_recv = int(m.group(2))
        host_dup = int(m.group(3) or 0)

    print("\n=== duplicate path attribution (B→A, radio A = receiver) ===")
    print(
        f"radio .9:  wifi_accept Δ={acc1 - acc0}  udp_fwd Δ={fwd1 - fwd0}  "
        f"promisc_data Δ={pd1 - pd0}  promisc_misc Δ={pm1 - pm0}"
    )
    print(f"pcap ports: {dict(port_hist)}  file={pcap}")
    print(
        f"eth :{FWD_PORT} (radio→manager MPDU):  pkts={eth_fwd.total}  "
        f"unique_app_seq={len(eth_fwd.seqs)}  dup_app_seq={eth_fwd.dup}"
    )
    print(
        f"eth :{HOST_PORT} (manager→bw_test):     pkts={eth_host.total}  "
        f"unique_app_seq={len(eth_host.seqs)}  dup_app_seq={eth_host.dup}"
    )
    if host_sent is not None:
        print(
            f"bw_test listener: sent={host_sent} unique_recv={host_recv} "
            f"dup_ignored={host_dup} total_recv≈{host_recv + (host_dup or 0)}"
        )

    if eth_fwd.dup > 0:
        print(
            "→ duplicate app-seq already on Ethernet forward "
            f"(:{FWD_PORT}): radio likely forwards same MPDU more than once "
            "per WiFi accept (promisc DATA+MISC or double enqueue)."
        )
    elif eth_host.dup > 0 and eth_fwd.dup == 0:
        print(
            f"→ forward wire unique but :{HOST_PORT} has dup: manager "
            "udp_endpoint / scheduler demux duplicates (extra Ethernet "
            "only on loopback toward the test harness)."
        )
    elif (host_dup or 0) > 0 and eth_fwd.dup == 0 and eth_host.dup == 0:
        print(
            "→ pcap missed loopback or timing; check bw_test dup_ignored vs "
            "eth :9001 counts."
        )

    ratio = ""
    if fwd1 > fwd0 and host_dup:
        ratio = f"  host_dup/udp_fwd={host_dup / max(1, fwd1 - fwd0):.2f}"
    if acc1 > acc0 and eth_fwd.total:
        ratio += f"  eth_fwd_pkts/wifi_accept={eth_fwd.total / max(1, acc1 - acc0):.2f}"
    if ratio:
        print(f"ratios:{ratio}")

    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
