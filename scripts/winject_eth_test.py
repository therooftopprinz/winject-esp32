#!/usr/bin/env python3
"""Ethernet integrity / flood bench for WInject.

Console RTT integrity (capped by request/reply):

  >> ether_test_tx size=<size>
  << ok sn=<sn> data=<binary>
  >> ether_test_rx sn=<sn> data=<binary>
  << ok last_sn=<sn> gap=<n>

Dedicated flood bench (pushes EMAC/lwIP toward link rate):

  UDP :2223  magic=0xEB01 LE, size u16 LE, sn u32 LE, payload...
  >> ether_bench_tx to=<host>:<port> size=<8-1472> count=<n>
  >> ether_bench_rx
  >> ether_bench_status
  >> ether_bench_stop

    # integrity (console RTT)
    python3 scripts/winject_eth_test.py --device 192.168.253.14
    python3 scripts/winject_eth_test.py --device 192.168.253.14 --window 4

    # flood bench (link saturation)
    python3 scripts/winject_eth_test.py --device 192.168.253.14 --bench
    python3 scripts/winject_eth_test.py --device 192.168.253.14 --bench-tx --count 20000 --size 1472
    python3 scripts/winject_eth_test.py --device 192.168.253.14 --bench-rx --count 20000
"""

from __future__ import annotations

import argparse
import select
import socket
import struct
import sys
import time

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

CONSOLE_PORT = 22
BENCH_PORT = 2223
BENCH_MAGIC = 0xEB01
BENCH_HDR = 8
DEFAULT_SIZE = 1400
DEFAULT_COUNT = 500
DEFAULT_WINDOW = 32
ETHER_TEST_MAX = 1400
ETHER_BENCH_MAX = 1472


def make_pattern(sn: int, size: int) -> bytes:
    return bytes(((sn + i) & 0xFF) for i in range(size))


def mbps(nbytes: int, dt: float) -> float:
    if dt <= 0.0 or nbytes <= 0:
        return 0.0
    return (nbytes * 8.0) / dt / 1_000_000.0


def make_bench_pkt(sn: int, size: int) -> bytes:
    if size < BENCH_HDR:
        raise ValueError("size too small")
    body = bytearray(size)
    struct.pack_into("<HHI", body, 0, BENCH_MAGIC, size, sn & 0xFFFFFFFF)
    # payload left as zeros — device TX uses 0xA5 fill; RX checks header only
    for i in range(BENCH_HDR, size):
        body[i] = 0xA5
    return bytes(body)


def parse_bench_hdr(data: bytes) -> tuple[int, int] | None:
    if len(data) < BENCH_HDR:
        return None
    magic, size, sn = struct.unpack_from("<HHI", data, 0)
    if magic != BENCH_MAGIC or size != len(data):
        return None
    return sn, size


def parse_tx_reply(raw: bytes) -> tuple[int, bytes]:
    prefix = b"ok sn="
    marker = b" data="
    if not raw.startswith(prefix):
        raise ValueError(f"bad tx reply prefix: {raw[:64]!r}")
    idx = raw.find(marker)
    if idx < 0:
        raise ValueError(f"missing data= in tx reply: {raw[:64]!r}")
    sn = int(raw[len(prefix) : idx])
    data = raw[idx + len(marker) :]
    return sn, data


def parse_rx_reply(raw: bytes) -> tuple[int, int]:
    text = raw.decode("utf-8", "replace").strip()
    if not text.startswith("ok "):
        raise ValueError(f"bad rx reply: {text!r}")
    last_sn = None
    gap = None
    for tok in text.split():
        if tok.startswith("last_sn="):
            last_sn = int(tok.split("=", 1)[1])
        elif tok.startswith("gap="):
            gap = int(tok.split("=", 1)[1])
    if last_sn is None or gap is None:
        raise ValueError(f"missing last_sn/gap: {text!r}")
    return last_sn, gap


def parse_status_kv(raw: bytes) -> dict[str, str]:
    text = raw.decode("utf-8", "replace").strip()
    if not text.startswith("ok"):
        raise ValueError(f"bad status: {text!r}")
    out: dict[str, str] = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def console_exchange(
    sock: socket.socket,
    dest: tuple[str, int],
    payload: bytes,
    timeout: float,
) -> bytes:
    sock.setblocking(True)
    sock.sendto(payload, dest)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            chunk, _ = sock.recvfrom(16384)
        except socket.timeout:
            continue
        if chunk:
            return chunk
    raise TimeoutError("console read timeout")


def recv_one(sock: socket.socket, timeout: float) -> bytes:
    ready, _, _ = select.select([sock], [], [], timeout)
    if not ready:
        raise TimeoutError("console read timeout")
    chunk, _ = sock.recvfrom(16384)
    if not chunk:
        raise TimeoutError("empty console reply")
    return chunk


def local_ipv4_for(device: str) -> str:
    """Pick the host address the radio will see as our source."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((device, 1))
        return s.getsockname()[0]
    finally:
        s.close()


def run_tx(
    sock: socket.socket,
    dest: tuple[str, int],
    *,
    size: int,
    count: int,
    window: int,
    timeout: float,
    verbose: bool,
) -> tuple[int, int]:
    ok = 0
    fail = 0
    req = f"ether_test_tx size={size}\n".encode()
    sent = 0
    in_flight = 0
    sock.setblocking(False)

    while sent < count or in_flight > 0:
        while in_flight < window and sent < count:
            sock.sendto(req, dest)
            sent += 1
            in_flight += 1

        try:
            raw = recv_one(sock, timeout)
        except TimeoutError as err:
            fail += in_flight
            print(f"tx fail: {err} (dropped {in_flight} in flight)")
            in_flight = 0
            break

        in_flight -= 1
        try:
            sn, data = parse_tx_reply(raw)
            expect = make_pattern(sn, size)
            if data != expect:
                fail += 1
                if verbose:
                    print(f"tx sn={sn} mismatch got={len(data)} expect={size}")
                continue
            ok += 1
            if verbose:
                print(f"tx sn={sn} size={len(data)} ok")
        except ValueError as err:
            fail += 1
            print(f"tx fail: {err}")

    sock.setblocking(True)
    return ok, fail


def run_rx(
    sock: socket.socket,
    dest: tuple[str, int],
    *,
    size: int,
    count: int,
    window: int,
    timeout: float,
    verbose: bool,
    start_sn: int = 1,
) -> tuple[int, int, int]:
    ok = 0
    fail = 0
    gap_total = 0
    sent = 0
    in_flight = 0
    pending: list[int] = []
    sock.setblocking(False)

    while sent < count or in_flight > 0:
        while in_flight < window and sent < count:
            sn = start_sn + sent
            payload = f"ether_test_rx sn={sn} data=".encode() + make_pattern(sn, size)
            sock.sendto(payload, dest)
            pending.append(sn)
            sent += 1
            in_flight += 1

        try:
            raw = recv_one(sock, timeout)
        except TimeoutError as err:
            fail += in_flight
            print(f"rx fail: {err} (dropped {in_flight} in flight)")
            in_flight = 0
            pending.clear()
            break

        in_flight -= 1
        expect_sn = pending.pop(0)
        try:
            last_sn, gap = parse_rx_reply(raw)
            if last_sn != expect_sn:
                fail += 1
                print(
                    f"rx sn mismatch reply last_sn={last_sn} expected={expect_sn}"
                )
                continue
            if gap != 0:
                print(f"rx sn={last_sn} unexpected gap={gap}")
            gap_total += gap
            ok += 1
            if verbose:
                print(f"rx sn={last_sn} gap={gap} ok")
        except ValueError as err:
            fail += 1
            print(f"rx fail: {err}")

    sock.setblocking(True)
    return ok, fail, gap_total


def bench_device_tx(
    console: socket.socket,
    dest: tuple[str, int],
    *,
    host_ip: str,
    host_port: int,
    size: int,
    count: int,
    timeout: float,
) -> tuple[int, int, float]:
    """Device floods → host. Returns ok, bad, mbps."""
    listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Bind all interfaces; still tell the device to target host_ip.
    listen.bind(("0.0.0.0", host_port))
    listen.setblocking(False)

    cmd = f"ether_bench_tx to={host_ip}:{host_port} size={size} count={count}\n"
    reply = console_exchange(console, dest, cmd.encode(), timeout)
    if not reply.startswith(b"ok"):
        raise RuntimeError(f"ether_bench_tx failed: {reply!r}")

    ok = 0
    bad = 0
    nbytes = 0
    t0 = time.monotonic()
    deadline = t0 + timeout + max(2.0, count * size * 8 / 5e7)
    last_st = {}

    while time.monotonic() < deadline and ok + bad < count:
        ready, _, _ = select.select([listen], [], [], 0.2)
        if ready:
            data, _ = listen.recvfrom(2048)
            parsed = parse_bench_hdr(data)
            if parsed is None:
                bad += 1
            else:
                ok += 1
                nbytes += len(data)
            continue

        last_st = parse_status_kv(
            console_exchange(console, dest, b"ether_bench_status\n", timeout)
        )
        tx_sent = int(last_st.get("tx_sent", "0"))
        running = last_st.get("tx_running") == "1"
        if not running and ok + bad >= tx_sent and tx_sent > 0:
            break
        if not running and time.monotonic() - t0 > 1.0:
            break

    # Final short drain.
    drain_end = time.monotonic() + 0.3
    while time.monotonic() < drain_end:
        ready, _, _ = select.select([listen], [], [], 0.05)
        if not ready:
            break
        data, _ = listen.recvfrom(2048)
        parsed = parse_bench_hdr(data)
        if parsed is None:
            bad += 1
        else:
            ok += 1
            nbytes += len(data)

    dt = time.monotonic() - t0
    listen.close()
    console_exchange(console, dest, b"ether_bench_stop\n", timeout)
    if ok == 0 and last_st:
        print(
            f"  device status tx_sent={last_st.get('tx_sent')} "
            f"tx_fail={last_st.get('tx_fail')} "
            f"tx_running={last_st.get('tx_running')}"
        )
    return ok, bad, mbps(nbytes, dt)


def bench_host_tx(
    console: socket.socket,
    dest: tuple[str, int],
    *,
    size: int,
    count: int,
    timeout: float,
    kbps: float,
) -> tuple[int, int, int, float]:
    """Host floods → device at a paced offer rate.

    An unpaced host sendto loop outruns the ESP32 UDP mailbox (~tens of
    packets) and drops almost everything before the app sees it (gap stays
    0 because lost packets never reach ether_bench_rx).
    """
    console_exchange(console, dest, b"ether_bench_rx\n", timeout)

    flood = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    flood.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    bench_dest = (dest[0], BENCH_PORT)

    pkt = bytearray(make_bench_pkt(0, size))
    interval = 0.0
    if kbps > 0:
        interval = (size * 8.0) / (kbps * 1000.0)

    t0 = time.monotonic()
    next_t = t0
    for sn in range(count):
        struct.pack_into("<I", pkt, 4, sn & 0xFFFFFFFF)
        flood.sendto(pkt, bench_dest)
        if interval > 0:
            next_t += interval
            sleep_for = next_t - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            elif sleep_for < -0.05:
                # Fell behind; resync so we don't burst the backlog.
                next_t = time.monotonic()
    dt_send = time.monotonic() - t0
    flood.close()

    # Poll until device counters settle (wire drain + mailbox).
    last_ok = -1
    stable = 0
    rx_ok = rx_bad = rx_gap = 0
    rx_bytes = 0
    deadline = time.monotonic() + max(2.0, timeout)
    while time.monotonic() < deadline:
        time.sleep(0.1)
        st = parse_status_kv(
            console_exchange(console, dest, b"ether_bench_status\n", timeout)
        )
        rx_ok = int(st.get("rx_ok", "0"))
        rx_bad = int(st.get("rx_bad", "0"))
        rx_gap = int(st.get("rx_gap", "0"))
        rx_bytes = int(st.get("rx_bytes", "0"))
        if rx_ok == last_ok:
            stable += 1
            if stable >= 3:
                break
        else:
            stable = 0
            last_ok = rx_ok

    console_exchange(console, dest, b"ether_bench_stop\n", timeout)
    rate = mbps(rx_bytes, dt_send)
    return rx_ok, rx_bad, rx_gap, rate


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="WInject Ethernet integrity / flood bench")
    p.add_argument("--device", "-d", default="192.168.253.14")
    p.add_argument("--port", type=int, default=CONSOLE_PORT)
    p.add_argument("--host", default="", help="host IP for bench-tx (auto if empty)")
    p.add_argument("--listen-port", type=int, default=2224, help="host listen for bench-tx")
    p.add_argument("--size", type=int, default=DEFAULT_SIZE)
    p.add_argument("--count", type=int, default=DEFAULT_COUNT)
    p.add_argument("--window", type=int, default=DEFAULT_WINDOW)
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument(
        "--kbps",
        type=float,
        default=90000.0,
        help="host→device bench offer in kbit/s (default 90000; 0 = unpaced blast)",
    )
    p.add_argument("--tx-only", action="store_true", help="console integrity TX only")
    p.add_argument("--rx-only", action="store_true", help="console integrity RX only")
    p.add_argument("--bench", action="store_true", help="run flood bench both ways")
    p.add_argument("--bench-tx", action="store_true", help="device→host flood only")
    p.add_argument("--bench-rx", action="store_true", help="host→device flood only")
    p.add_argument("--verbose", "-v", action="store_true")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    bench_mode = args.bench or args.bench_tx or args.bench_rx
    if args.bench:
        args.bench_tx = True
        args.bench_rx = True

    if bench_mode:
        if not BENCH_HDR <= args.size <= ETHER_BENCH_MAX:
            print(f"bench size must be {BENCH_HDR}..{ETHER_BENCH_MAX}", file=sys.stderr)
            return 2
    else:
        if not 1 <= args.size <= ETHER_TEST_MAX:
            print(f"size must be 1..{ETHER_TEST_MAX}", file=sys.stderr)
            return 2
    if args.count < 1:
        print("count must be >= 1", file=sys.stderr)
        return 2

    dest = (args.device, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    try:
        try:
            pong = console_exchange(sock, dest, b"ping\n", args.timeout)
        except TimeoutError:
            print(f"no reply from {args.device}:{args.port} (ping)", file=sys.stderr)
            return 1
        if b"pong" not in pong:
            print(f"unexpected ping reply: {pong!r}", file=sys.stderr)
            return 1

        if bench_mode:
            host_ip = args.host or local_ipv4_for(args.device)
            print(
                f"console {args.device}:{args.port} pong  "
                f"BENCH size={args.size} count={args.count} host={host_ip} "
                f"rx_offer={args.kbps:.0f} kbps"
            )
            failed = 0
            if args.bench_tx:
                print("-- ether_bench_tx (device → host flood)")
                ok, bad, rate = bench_device_tx(
                    sock,
                    dest,
                    host_ip=host_ip,
                    host_port=args.listen_port,
                    size=args.size,
                    count=args.count,
                    timeout=args.timeout,
                )
                print(f"tx  recv {ok}/{args.count}  bad={bad}  {rate:.2f} Mbps")
                if ok < args.count * 0.95:
                    failed += 1
            if args.bench_rx:
                print("-- ether_bench_rx (host → device flood)")
                ok, bad, gap, rate = bench_host_tx(
                    sock,
                    dest,
                    size=args.size,
                    count=args.count,
                    timeout=args.timeout,
                    kbps=args.kbps,
                )
                print(
                    f"rx  ok={ok}/{args.count}  bad={bad}  gap={gap}  "
                    f"{rate:.2f} Mbps"
                )
                if ok < args.count * 0.95:
                    failed += 1
            if failed:
                print(f"FAIL ({failed} directions under 95%)")
                return 1
            print("PASS")
            return 0

        # Console integrity path
        if args.tx_only and args.rx_only:
            print("use at most one of --tx-only / --rx-only", file=sys.stderr)
            return 2
        if args.window < 1:
            print("window must be >= 1", file=sys.stderr)
            return 2
        do_tx = not args.rx_only
        do_rx = not args.tx_only
        print(
            f"console {args.device}:{args.port} pong  "
            f"size={args.size} count={args.count} window={args.window}"
        )

        tx_ok = tx_fail = 0
        rx_ok = rx_fail = 0
        gap_total = 0
        if do_tx:
            print("-- ether_test_tx (device → host)")
            t0 = time.monotonic()
            tx_ok, tx_fail = run_tx(
                sock,
                dest,
                size=args.size,
                count=args.count,
                window=args.window,
                timeout=args.timeout,
                verbose=args.verbose,
            )
            dt = time.monotonic() - t0
            print(
                f"tx  {tx_ok}/{args.count} ok  fail={tx_fail}  "
                f"{dt:.2f}s  {mbps(tx_ok * args.size, dt):.2f} Mbps"
            )
        if do_rx:
            print("-- ether_test_rx (host → device)")
            t0 = time.monotonic()
            rx_ok, rx_fail, gap_total = run_rx(
                sock,
                dest,
                size=args.size,
                count=args.count,
                window=args.window,
                timeout=args.timeout,
                verbose=args.verbose,
            )
            dt = time.monotonic() - t0
            print(
                f"rx  {rx_ok}/{args.count} ok  fail={rx_fail}  "
                f"gap_sum={gap_total}  {dt:.2f}s  "
                f"{mbps(rx_ok * args.size, dt):.2f} Mbps"
            )
        failed = tx_fail + rx_fail
        if failed:
            print(f"FAIL ({failed} errors)")
            return 1
        print("PASS")
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
