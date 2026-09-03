#!/usr/bin/env python3
"""One-way air TX-RX latency through winject-manager on this host.

Same-host timestamps (time.monotonic_ns), so A->B / B->A do not need clock sync.
UDP: datagrams to manager bind ports. TCP: uint16 BE length-prefixed records
(same framing as tools/bw_test.py --tcp).
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bw_test as bw  # noqa: E402

MAGIC = b"LAT1"
HDR_FMT = "!4sIQ"  # magic, seq, t_ns
HDR_LEN = struct.calcsize(HDR_FMT)
MAX_PAYLOAD = bw.MAX_PAYLOAD


@dataclass
class LatStats:
    samples_us: list[float] = field(default_factory=list)
    seqs: set[int] = field(default_factory=set)
    nbytes: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)

    def add(self, seq: int, send_ns: int, recv_ns: int, n: int) -> None:
        lat_us = (recv_ns - send_ns) / 1000.0
        if lat_us < 0 or lat_us > 5_000_000:
            return
        with self.lock:
            self.samples_us.append(lat_us)
            self.seqs.add(seq)
            self.nbytes += n

    def snapshot(self) -> tuple[list[float], set[int], int]:
        with self.lock:
            return list(self.samples_us), set(self.seqs), self.nbytes

    def clear(self) -> None:
        with self.lock:
            self.samples_us.clear()
            self.seqs.clear()
            self.nbytes = 0


def make_probe(seq: int, size: int, t_ns: int | None = None) -> bytes:
    if size < HDR_LEN:
        raise SystemExit(f"--size must be >= {HDR_LEN}")
    if t_ns is None:
        t_ns = time.monotonic_ns()
    body = struct.pack(HDR_FMT, MAGIC, seq, t_ns)
    if size > HDR_LEN:
        body += bytes(size - HDR_LEN)
    return body


def parse_probe(data: bytes) -> tuple[int, int] | None:
    if len(data) < HDR_LEN or data[:4] != MAGIC:
        return None
    _magic, seq, t_ns = struct.unpack(HDR_FMT, data[:HDR_LEN])
    return seq, t_ns


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def summarize(xs: list[float]) -> dict[str, float]:
    if not xs:
        return {
            "n": 0.0,
            "min": float("nan"),
            "p50": float("nan"),
            "p95": float("nan"),
            "p99": float("nan"),
            "max": float("nan"),
            "avg": float("nan"),
            "stdev": float("nan"),
        }
    n = len(xs)
    avg = sum(xs) / n
    var = sum((x - avg) ** 2 for x in xs) / n
    return {
        "n": float(n),
        "min": min(xs),
        "p50": percentile(xs, 50),
        "p95": percentile(xs, 95),
        "p99": percentile(xs, 99),
        "max": max(xs),
        "avg": avg,
        "stdev": math.sqrt(var),
    }


def fmt_ms(us: float) -> str:
    if math.isnan(us):
        return "   n/a"
    return f"{us / 1000.0:6.3f}"


def print_result(label: str, direction: str, sent: int, stats: LatStats) -> dict:
    samples, seqs, _nbytes = stats.snapshot()
    s = summarize(samples)
    loss = 100.0 * max(0, sent - len(seqs)) / sent if sent else 100.0
    print(
        f"RESULT {label} {direction} sent={sent} recv={int(s['n'])} "
        f"loss={loss:.1f}%  min={fmt_ms(s['min'])} p50={fmt_ms(s['p50'])} "
        f"p95={fmt_ms(s['p95'])} p99={fmt_ms(s['p99'])} max={fmt_ms(s['max'])} "
        f"avg={fmt_ms(s['avg'])} stdev={fmt_ms(s['stdev'])} ms"
    )
    return {
        "label": label,
        "direction": direction,
        "sent": sent,
        "recv": int(s["n"]),
        "loss": loss,
        **s,
    }


class UdpListener:
    def __init__(self, bind_ip: str, port: int, stats: LatStats) -> None:
        self.stats = stats
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self._sock.bind((bind_ip, port))
        self._sock.settimeout(0.05)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sock.close()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data, _addr = self._sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            parsed = parse_probe(data)
            if parsed is None:
                continue
            seq, send_ns = parsed
            self.stats.add(seq, send_ns, time.monotonic_ns(), len(data))


class TcpListener:
    def __init__(self, bind_ip: str, port: int, stats: LatStats) -> None:
        self.stats = stats
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((bind_ip, port))
        self._sock.listen(1)
        self._sock.settimeout(0.2)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._client: socket.socket | None = None

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        if self._client is not None:
            try:
                self._client.close()
            except OSError:
                pass
        self._sock.close()

    def _consume(self, buf: bytearray) -> None:
        while len(buf) >= 2:
            plen = struct.unpack("!H", buf[:2])[0]
            if plen > MAX_PAYLOAD or plen == 0:
                del buf[0]
                continue
            if len(buf) < 2 + plen:
                return
            data = bytes(buf[2 : 2 + plen])
            del buf[: 2 + plen]
            parsed = parse_probe(data)
            if parsed is None:
                continue
            seq, send_ns = parsed
            self.stats.add(seq, send_ns, time.monotonic_ns(), len(data))

    def _read_client(self, conn: socket.socket) -> None:
        conn.settimeout(0.2)
        buf = bytearray()
        while not self._stop.is_set():
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
            self._consume(buf)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            if self._client is not None:
                try:
                    self._client.close()
                except OSError:
                    pass
            self._client = conn
            self._read_client(conn)


def send_udp(sock: socket.socket, dest: tuple[str, int], seq: int, size: int) -> None:
    sock.sendto(make_probe(seq, size), dest)


def send_tcp(sock: socket.socket, seq: int, size: int) -> None:
    bw.send_tcp_record(sock, make_probe(seq, size))


def paced_send(
    count: int,
    interval_s: float,
    send_one,
) -> None:
    next_t = time.monotonic()
    for seq in range(count):
        now = time.monotonic()
        if interval_s > 0 and now < next_t:
            time.sleep(next_t - now)
        send_one(seq)
        if interval_s > 0:
            next_t += interval_s
            if next_t < time.monotonic() - interval_s:
                next_t = time.monotonic()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="manager air TX-RX one-way latency")
    p.add_argument("--tcp", action="store_true", help="length-prefixed TCP via manager")
    p.add_argument("--send-a", type=int, default=bw.TCP_SEND_A, help="A->B send port")
    p.add_argument("--send-b", type=int, default=bw.TCP_SEND_B, help="B->A send port")
    p.add_argument("--recv-a", type=int, default=bw.HOST_PORT_A, help="B->A listen port")
    p.add_argument("--recv-b", type=int, default=bw.HOST_PORT_B, help="A->B listen port")
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--count", type=int, default=400, help="measured probes per direction")
    p.add_argument("--warmup", type=int, default=40, help="probes sent before measuring")
    p.add_argument("--interval-ms", type=float, default=1.0, help="send spacing (0 = blast)")
    p.add_argument("--size", type=int, default=64, help=f"payload bytes ({HDR_LEN}..{MAX_PAYLOAD})")
    p.add_argument("--drain", type=float, default=0.5, help="seconds to wait after sending")
    p.add_argument("--ba", action="store_true", help="also measure B->A")
    p.add_argument("--label", default="lat", help="tag printed on RESULT lines")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if args.size < HDR_LEN or args.size > MAX_PAYLOAD:
        raise SystemExit(f"--size must be {HDR_LEN}..{MAX_PAYLOAD}")
    if args.count < 1:
        raise SystemExit("--count must be >= 1")
    if args.interval_ms < 0:
        raise SystemExit("--interval-ms must be >= 0")

    interval_s = args.interval_ms / 1000.0
    proto = "tcp" if args.tcp else "udp"
    print(
        f"lat_test {args.label} {proto} size={args.size} count={args.count} "
        f"warmup={args.warmup} interval={args.interval_ms:g}ms drain={args.drain:g}s"
    )

    stats_ab = LatStats()
    stats_ba = LatStats()
    if args.tcp:
        listen_b = TcpListener(args.bind, args.recv_b, stats_ab)
        listen_a = TcpListener(args.bind, args.recv_a, stats_ba) if args.ba else None
    else:
        listen_b = UdpListener(args.bind, args.recv_b, stats_ab)
        listen_a = UdpListener(args.bind, args.recv_a, stats_ba) if args.ba else None
    listen_b.start()
    if listen_a is not None:
        listen_a.start()
    time.sleep(0.15)

    dest_a = (args.bind, args.send_a)
    dest_b = (args.bind, args.send_b)
    udp_sock: socket.socket | None = None
    tcp_a: socket.socket | None = None
    tcp_b: socket.socket | None = None
    try:
        if args.tcp:
            tcp_a = bw.tcp_connect(dest_a[0], dest_a[1])
            time.sleep(0.25)
            if args.ba:
                tcp_b = bw.tcp_connect(dest_b[0], dest_b[1])
                time.sleep(0.25)

            def send_ab(seq: int) -> None:
                send_tcp(tcp_a, seq, args.size)

            def send_ba(seq: int) -> None:
                assert tcp_b is not None
                send_tcp(tcp_b, seq, args.size)
        else:
            udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

            def send_ab(seq: int) -> None:
                send_udp(udp_sock, dest_a, seq, args.size)

            def send_ba(seq: int) -> None:
                send_udp(udp_sock, dest_b, seq, args.size)

        def run_dir(name: str, send_one, stats: LatStats) -> dict:
            stats.clear()
            if args.warmup > 0:
                paced_send(args.warmup, interval_s, send_one)
                time.sleep(max(args.drain, 0.15))
                stats.clear()
            paced_send(args.count, interval_s, send_one)
            time.sleep(args.drain)
            return print_result(args.label, name, args.count, stats)

        rows = [run_dir("A->B", send_ab, stats_ab)]
        if args.ba:
            rows.append(run_dir("B->A", send_ba, stats_ba))
    finally:
        listen_b.stop()
        if listen_a is not None:
            listen_a.stop()
        if udp_sock is not None:
            udp_sock.close()
        if tcp_a is not None:
            tcp_a.close()
        if tcp_b is not None:
            tcp_b.close()

    failed = any(r["recv"] == 0 or r["loss"] > 50.0 for r in rows)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
