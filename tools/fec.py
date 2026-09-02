#!/usr/bin/env python3
"""Host-side Reed-Solomon FEC proxy for WInject-ESP32.

Sits between an app and a winject UDP port. The firmware still treats each
datagram as one opaque 802.11 body (max 1476 bytes). This process adds or
strips FEC.

    python3 tools/fec.py --encode --from 9100 --to 192.168.253.11:9000
    python3 tools/fec.py --decode --from 9002 --to 127.0.0.1:9101

Default is packet-block erasure FEC (k data + n-k parity datagrams).
--intra appends per-frame RS parity instead (useful only with
set_allow_failed_crc). Requires: pip install reedsolo
"""

from __future__ import annotations

import argparse
import select
import socket
import struct
import sys
import time
from collections import OrderedDict
from dataclasses import dataclass, field

try:
    from reedsolo import ReedSolomonError, RSCodec
except ImportError:
    sys.stderr.write("fec.py requires reedsolo. Install with: pip install reedsolo\n")
    raise SystemExit(1)

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

MAX_PAYLOAD = 1476
BLOCK_HDR = 8
LEN_PREFIX = 2
MAX_BLOCK_ORIG = MAX_PAYLOAD - BLOCK_HDR - LEN_PREFIX  # 1466
INTRA_HDR = 2
MAGIC_BLOCK = 0xF1
MAGIC_INTRA = 0xF2
VERSION = 1
FLAG_PARITY = 0x01
DEFAULT_K = 8
DEFAULT_N = 12
DEFAULT_NSYM = 32
DEFAULT_TIMEOUT_MS = 20
DONE_MAX = 128
BLOCK_MAX = 64
RECV_MAX = 2048
STATS_PERIOD_S = 2.0

_CODECS: dict[int, RSCodec] = {}


def codec(nsym: int) -> RSCodec:
    c = _CODECS.get(nsym)
    if c is None:
        if nsym < 1:
            raise ValueError("nsym must be >= 1")
        c = RSCodec(nsym)
        _CODECS[nsym] = c
    return c


@dataclass
class Stats:
    in_pkts: int = 0
    in_bytes: int = 0
    out_pkts: int = 0
    out_bytes: int = 0
    oversized: int = 0
    recovered: int = 0
    dropped: int = 0
    passthrough: int = 0
    blocks: int = 0
    decode_fail: int = 0

    def line(self, mode: str, extra: str = "") -> str:
        parts = [
            f"fec {mode}:",
            f"in={self.in_pkts}",
            f"out={self.out_pkts}",
            f"blocks={self.blocks}",
            f"recovered={self.recovered}",
            f"drop={self.dropped}",
            f"oversized={self.oversized}",
            f"pass={self.passthrough}",
            f"fail={self.decode_fail}",
        ]
        if extra:
            parts.append(extra)
        return " ".join(parts)


@dataclass
class BlockBuf:
    k: int
    n: int
    fragments: dict[int, bytes] = field(default_factory=dict)
    first_seen: float = 0.0


@dataclass
class EncodeState:
    intra: bool
    k: int
    n: int
    nsym: int
    timeout_s: float
    pending: list[bytes] = field(default_factory=list)
    deadline: float | None = None
    block_id: int = 0
    stats: Stats = field(default_factory=Stats)


@dataclass
class DecodeState:
    blocks: dict[int, BlockBuf] = field(default_factory=dict)
    done: OrderedDict[int, None] = field(default_factory=OrderedDict)
    stats: Stats = field(default_factory=Stats)


def parse_endpoint(text: str) -> tuple[str, int]:
    if ":" not in text:
        raise argparse.ArgumentTypeError(f"endpoint must be host:port, got {text!r}")
    host, _, port_s = text.rpartition(":")
    if not host:
        raise argparse.ArgumentTypeError(f"endpoint missing host: {text!r}")
    try:
        port = int(port_s)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"endpoint port is not an int: {text!r}") from exc
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError(f"endpoint port out of range: {port}")
    return host, port


def validate_kn(k: int, n: int) -> None:
    if not 1 <= k < n <= 255:
        raise SystemExit(f"need 1 <= k < n <= 255, got k={k} n={n}")


def pack_block_header(block_id: int, index: int, k: int, n: int, flags: int) -> bytes:
    return struct.pack(
        "!BBHBBBB",
        MAGIC_BLOCK,
        VERSION,
        block_id & 0xFFFF,
        index,
        k,
        n,
        flags,
    )


def unpack_block_header(data: bytes) -> tuple[int, int, int, int, int, int] | None:
    if len(data) < BLOCK_HDR:
        return None
    magic, version, block_id, index, k, n, flags = struct.unpack("!BBHBBBB", data[:BLOCK_HDR])
    if magic != MAGIC_BLOCK or version != VERSION:
        return None
    if not 1 <= k < n <= 255 or index >= n:
        return None
    return block_id, index, k, n, flags, BLOCK_HDR


def encode_block(packets: list[bytes], k: int, n: int, block_id: int) -> list[bytes]:
    """Column-wise systematic RS: k data rows (+ empty pads) become n fragments."""
    if len(packets) > k:
        raise ValueError("too many packets for k")
    rows: list[bytes] = []
    max_row = 0
    padded_pkts = list(packets) + [b""] * (k - len(packets))
    for pkt in padded_pkts:
        row = struct.pack("!H", len(pkt)) + pkt
        rows.append(row)
        if len(row) > max_row:
            max_row = len(row)
    if max_row + BLOCK_HDR > MAX_PAYLOAD:
        raise ValueError("block row exceeds max payload")

    padded = [row + b"\x00" * (max_row - len(row)) for row in rows]
    rsc = codec(n - k)
    bodies = [bytearray(max_row) for _ in range(n)]
    col = bytearray(k)
    for c in range(max_row):
        for r in range(k):
            col[r] = padded[r][c]
        enc = rsc.encode(col)
        for r in range(n):
            bodies[r][c] = enc[r]

    out: list[bytes] = []
    for i in range(n):
        flags = FLAG_PARITY if i >= k else 0
        out.append(pack_block_header(block_id, i, k, n, flags) + bytes(bodies[i]))
    return out


def _parse_rows(rows: list[bytes]) -> list[bytes] | None:
    originals: list[bytes] = []
    for row in rows:
        if len(row) < LEN_PREFIX:
            return None
        orig_len = struct.unpack("!H", row[:LEN_PREFIX])[0]
        if orig_len == 0:
            continue
        if LEN_PREFIX + orig_len > len(row):
            return None
        originals.append(bytes(row[LEN_PREFIX : LEN_PREFIX + orig_len]))
    return originals


def decode_block(fragments: dict[int, bytes], k: int, n: int) -> tuple[list[bytes], int] | None:
    """Recover original datagrams. Returns (payloads, recovered_count) or None."""
    if len(fragments) < k:
        return None
    body_len = max(len(body) for body in fragments.values())
    good = {i: body for i, body in fragments.items() if len(body) == body_len}
    if len(good) < k:
        return None

    missing_data = [i for i in range(k) if i not in good]
    if not missing_data and all(i in good for i in range(k)):
        parsed = _parse_rows([good[i] for i in range(k)])
        if parsed is None:
            return None
        return parsed, 0

    erase = [i for i in range(n) if i not in good]
    if len(erase) > n - k:
        return None

    rsc = codec(n - k)
    recovered_rows = [bytearray(body_len) for _ in range(k)]
    buf = bytearray(n)
    try:
        for c in range(body_len):
            buf[:] = b"\x00" * n
            for i, body in good.items():
                buf[i] = body[c]
            dec = rsc.decode(buf, erase_pos=erase, only_erasures=True)[0]
            for r in range(k):
                recovered_rows[r][c] = dec[r]
    except ReedSolomonError:
        return None

    parsed = _parse_rows([bytes(row) for row in recovered_rows])
    if parsed is None:
        return None
    recovered = 0
    for i in missing_data:
        orig_len = struct.unpack("!H", bytes(recovered_rows[i][:LEN_PREFIX]))[0]
        if orig_len > 0:
            recovered += 1
    return parsed, recovered


def encode_intra(payload: bytes, nsym: int) -> bytes | None:
    if not payload:
        return bytes([MAGIC_INTRA, nsym])
    try:
        encoded = codec(nsym).encode(payload)
    except Exception:
        return None
    pkt = bytes([MAGIC_INTRA, nsym]) + bytes(encoded)
    if len(pkt) > MAX_PAYLOAD:
        return None
    return pkt


def decode_intra(data: bytes) -> bytes | None:
    if len(data) < INTRA_HDR or data[0] != MAGIC_INTRA:
        return None
    nsym = data[1]
    if nsym < 1:
        return None
    body = data[INTRA_HDR:]
    if not body:
        return b""
    if len(body) < nsym:
        return None
    try:
        return bytes(codec(nsym).decode(body)[0])
    except ReedSolomonError:
        return None


def encode_feed(state: EncodeState, payload: bytes) -> list[bytes]:
    """Consume one original datagram; return FEC datagrams to send (maybe none yet)."""
    state.stats.in_pkts += 1
    state.stats.in_bytes += len(payload)
    if state.intra:
        if len(payload) > MAX_PAYLOAD - INTRA_HDR:
            state.stats.oversized += 1
            return []
        pkt = encode_intra(payload, state.nsym)
        if pkt is None:
            state.stats.oversized += 1
            return []
        state.stats.out_pkts += 1
        state.stats.out_bytes += len(pkt)
        return [pkt]

    if len(payload) > MAX_BLOCK_ORIG:
        state.stats.oversized += 1
        return []
    state.pending.append(payload)
    now = time.monotonic()
    if state.deadline is None:
        state.deadline = now + state.timeout_s
    if len(state.pending) >= state.k:
        return encode_flush(state)
    return []


def encode_flush(state: EncodeState) -> list[bytes]:
    if not state.pending:
        return []
    pkts = encode_block(state.pending, state.k, state.n, state.block_id)
    state.block_id = (state.block_id + 1) & 0xFFFF
    state.pending.clear()
    state.deadline = None
    state.stats.blocks += 1
    state.stats.out_pkts += len(pkts)
    state.stats.out_bytes += sum(len(p) for p in pkts)
    return pkts


def _drop_oldest_block(state: DecodeState) -> None:
    if not state.blocks:
        return
    oldest_id = min(state.blocks, key=lambda bid: state.blocks[bid].first_seen)
    state.blocks.pop(oldest_id)
    state.stats.dropped += 1


def _mark_done(state: DecodeState, block_id: int) -> None:
    state.done[block_id] = None
    while len(state.done) > DONE_MAX:
        state.done.popitem(last=False)


def decode_feed(state: DecodeState, data: bytes) -> list[bytes]:
    """Consume one air datagram; return original payloads to forward."""
    state.stats.in_pkts += 1
    state.stats.in_bytes += len(data)
    if not data:
        state.stats.dropped += 1
        return []

    magic = data[0]
    if magic == MAGIC_INTRA:
        payload = decode_intra(data)
        if payload is None:
            state.stats.decode_fail += 1
            state.stats.dropped += 1
            return []
        state.stats.out_pkts += 1
        state.stats.out_bytes += len(payload)
        return [payload]

    if magic != MAGIC_BLOCK:
        state.stats.passthrough += 1
        state.stats.out_pkts += 1
        state.stats.out_bytes += len(data)
        return [data]

    hdr = unpack_block_header(data)
    if hdr is None:
        state.stats.decode_fail += 1
        state.stats.dropped += 1
        return []
    block_id, index, k, n, _flags, hlen = hdr
    body = data[hlen:]
    if block_id in state.done:
        return []

    buf = state.blocks.get(block_id)
    if buf is None:
        while len(state.blocks) >= BLOCK_MAX:
            _drop_oldest_block(state)
        buf = BlockBuf(k=k, n=n, first_seen=time.monotonic())
        state.blocks[block_id] = buf
    if buf.k != k or buf.n != n:
        state.stats.decode_fail += 1
        state.stats.dropped += 1
        return []
    if index in buf.fragments:
        return []
    buf.fragments[index] = body
    if len(buf.fragments) < k:
        return []

    result = decode_block(buf.fragments, k, n)
    state.blocks.pop(block_id, None)
    _mark_done(state, block_id)
    if result is None:
        state.stats.decode_fail += 1
        state.stats.dropped += 1
        return []
    payloads, recovered = result
    state.stats.blocks += 1
    state.stats.recovered += recovered
    state.stats.out_pkts += len(payloads)
    state.stats.out_bytes += sum(len(p) for p in payloads)
    return payloads


def bind_udp(host: str, port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
    sock.bind((host, port))
    sock.setblocking(False)
    return sock


def maybe_print_stats(mode: str, stats: Stats, verbose: bool, last: list[float], extra: str = "") -> None:
    if not verbose:
        return
    now = time.monotonic()
    if now - last[0] < STATS_PERIOD_S:
        return
    last[0] = now
    print(stats.line(mode, extra), file=sys.stderr)


def run_encode(args: argparse.Namespace, dest: tuple[str, int]) -> None:
    sock = bind_udp(args.bind, args.from_port)
    state = EncodeState(
        intra=args.intra,
        k=args.k,
        n=args.n,
        nsym=args.nsym,
        timeout_s=args.timeout / 1000.0,
    )
    last_stats = [time.monotonic()]
    print(
        f"fec encode listening {args.bind}:{args.from_port} -> {dest[0]}:{dest[1]}"
        f" intra={args.intra} k={args.k} n={args.n} nsym={args.nsym}",
        file=sys.stderr,
    )
    try:
        while True:
            timeout = None
            if state.pending and state.deadline is not None:
                timeout = max(0.0, state.deadline - time.monotonic())
            readable, _, _ = select.select([sock], [], [], timeout)
            to_send: list[bytes] = []
            if readable:
                try:
                    while True:
                        data, _addr = sock.recvfrom(RECV_MAX)
                        to_send.extend(encode_feed(state, data))
                except BlockingIOError:
                    pass
            elif state.pending:
                to_send.extend(encode_flush(state))
            for pkt in to_send:
                sock.sendto(pkt, dest)
            extra = f"pending={len(state.pending)}" if not args.intra else ""
            maybe_print_stats("encode", state.stats, args.verbose, last_stats, extra)
    except KeyboardInterrupt:
        if state.pending:
            for pkt in encode_flush(state):
                sock.sendto(pkt, dest)
        print(state.stats.line("encode"), file=sys.stderr)
    finally:
        sock.close()


def run_decode(args: argparse.Namespace, dest: tuple[str, int]) -> None:
    sock = bind_udp(args.bind, args.from_port)
    state = DecodeState()
    last_stats = [time.monotonic()]
    print(
        f"fec decode listening {args.bind}:{args.from_port} -> {dest[0]}:{dest[1]}",
        file=sys.stderr,
    )
    try:
        while True:
            timeout = STATS_PERIOD_S if args.verbose else None
            readable, _, _ = select.select([sock], [], [], timeout)
            if readable:
                try:
                    while True:
                        data, _addr = sock.recvfrom(RECV_MAX)
                        for payload in decode_feed(state, data):
                            sock.sendto(payload, dest)
                except BlockingIOError:
                    pass
            maybe_print_stats(
                "decode",
                state.stats,
                args.verbose,
                last_stats,
                extra=f"pending={len(state.blocks)}",
            )
    except KeyboardInterrupt:
        print(state.stats.line("decode"), file=sys.stderr)
    finally:
        sock.close()


def _expect(cond: bool, msg: str, failures: list[str]) -> None:
    if not cond:
        failures.append(msg)


def self_test() -> int:
    failures: list[str] = []

    orig = [bytes([i & 0xFF]) * (40 + i) for i in range(DEFAULT_K)]
    pkts = encode_block(orig, DEFAULT_K, DEFAULT_N, 1)
    _expect(len(pkts) == DEFAULT_N, f"block emit n={len(pkts)}", failures)
    _expect(all(len(p) <= MAX_PAYLOAD for p in pkts), "block packet too long", failures)

    def frags_from(packets: list[bytes], drop: set[int]) -> dict[int, bytes]:
        out: dict[int, bytes] = {}
        for p in packets:
            hdr = unpack_block_header(p)
            assert hdr is not None
            block_id, index, k, n, _flags, hlen = hdr
            if index in drop:
                continue
            out[index] = p[hlen:]
        return out

    got = decode_block(frags_from(pkts, set()), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == orig and got[1] == 0, "block no-loss roundtrip", failures)

    got = decode_block(frags_from(pkts, {8, 9, 10, 11}), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == orig and got[1] == 0, "block drop all parity", failures)

    got = decode_block(frags_from(pkts, {0, 1, 2, 3}), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == orig, "block drop 4 data", failures)
    _expect(got is not None and got[1] == 4, f"block recovered={got[1] if got else None} want 4", failures)

    got = decode_block(frags_from(pkts, {1, 4, 9, 11}), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == orig, "block mixed 4 losses", failures)

    got = decode_block(frags_from(pkts, {0, 1, 2, 3, 4}), DEFAULT_K, DEFAULT_N)
    _expect(got is None, "block 5 losses should fail", failures)

    partial = encode_block([b"alpha", b"beta", b"gamma"], DEFAULT_K, DEFAULT_N, 2)
    got = decode_block(frags_from(partial, {5, 6, 7, 10}), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == [b"alpha", b"beta", b"gamma"], "block partial flush pads", failures)

    max_payload = bytes(range(256)) * (MAX_BLOCK_ORIG // 256) + bytes(range(MAX_BLOCK_ORIG % 256))
    _expect(len(max_payload) == MAX_BLOCK_ORIG, "max orig size", failures)
    max_block = encode_block([max_payload], DEFAULT_K, DEFAULT_N, 3)
    _expect(all(len(p) <= MAX_PAYLOAD for p in max_block), "max block fits 1476", failures)
    got = decode_block(frags_from(max_block, {0, 2, 8, 9}), DEFAULT_K, DEFAULT_N)
    _expect(got is not None and got[0] == [max_payload], "max payload roundtrip with losses", failures)

    enc = EncodeState(intra=False, k=DEFAULT_K, n=DEFAULT_N, nsym=DEFAULT_NSYM, timeout_s=0.02)
    dec = DecodeState()
    forwarded: list[bytes] = []
    for i, payload in enumerate(orig):
        air = encode_feed(enc, payload)
        if i < DEFAULT_K - 1:
            _expect(air == [], f"encode should buffer until k, i={i}", failures)
        for pkt in air:
            forwarded.extend(decode_feed(dec, pkt))
    _expect(forwarded == orig, "encode/decode feed full block", failures)
    _expect(enc.stats.blocks == 1 and dec.stats.blocks == 1, "feed block counts", failures)

    oversized = encode_feed(enc, b"x" * (MAX_BLOCK_ORIG + 1))
    _expect(oversized == [] and enc.stats.oversized == 1, "block oversized dropped", failures)

    intra_enc = EncodeState(intra=True, k=DEFAULT_K, n=DEFAULT_N, nsym=DEFAULT_NSYM, timeout_s=0.02)
    intra_dec = DecodeState()
    sample = b"hello-intra-fec" + bytes(range(200))
    air = encode_feed(intra_enc, sample)
    _expect(len(air) == 1 and air[0][0] == MAGIC_INTRA, "intra encode one packet", failures)
    back = decode_feed(intra_dec, air[0])
    _expect(back == [sample], "intra roundtrip", failures)

    corrupted = bytearray(air[0])
    for off in range(10, 26):
        corrupted[off] ^= 0x5A
    repaired = decode_feed(intra_dec, bytes(corrupted))
    _expect(repaired == [sample], "intra corrects byte errors", failures)

    empty_air = encode_feed(intra_enc, b"")
    _expect(decode_feed(intra_dec, empty_air[0]) == [b""], "intra empty payload", failures)

    too_big = encode_feed(intra_enc, b"y" * (MAX_PAYLOAD - INTRA_HDR + 1))
    _expect(too_big == [] and intra_enc.stats.oversized >= 1, "intra oversized dropped", failures)

    passthrough = decode_feed(DecodeState(), b"\x00plain-udp")
    _expect(passthrough == [b"\x00plain-udp"], "unknown magic passthrough", failures)

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        print(f"self-test: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("self-test: ok")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="WInject host-side Reed-Solomon FEC UDP proxy")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--encode", action="store_true", help="FEC-encode datagrams toward winject")
    mode.add_argument("--decode", action="store_true", help="FEC-decode datagrams from winject")
    p.add_argument("--from", dest="from_port", type=int, metavar="PORT", help="UDP bind port")
    p.add_argument("--to", dest="to_ep", metavar="HOST:PORT", help="IPv4 destination host:port")
    p.add_argument("--k", type=int, default=DEFAULT_K, help="data packets per block (default 8)")
    p.add_argument("--n", type=int, default=DEFAULT_N, help="total packets per block (default 12)")
    p.add_argument("--nsym", type=int, default=DEFAULT_NSYM, help="intra RS parity bytes (default 32)")
    p.add_argument("--intra", action="store_true", help="per-frame RS instead of packet-block FEC")
    p.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_MS,
        metavar="MS",
        help="encode partial-block flush timeout in ms (default 20)",
    )
    p.add_argument("--bind", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    p.add_argument("--verbose", action="store_true", help="print stats every 2s")
    p.add_argument("--self-test", action="store_true", help="run encode/decode checks and exit")
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.self_test:
        return self_test()
    if args.encode == args.decode:
        print("specify exactly one of --encode or --decode (or --self-test)", file=sys.stderr)
        return 2
    if args.from_port is None or args.to_ep is None:
        print("--from PORT and --to HOST:PORT are required", file=sys.stderr)
        return 2
    if not 1 <= args.from_port <= 65535:
        print(f"bind port out of range: {args.from_port}", file=sys.stderr)
        return 2
    if args.nsym < 1 or args.nsym > 254:
        print(f"nsym must be 1..254, got {args.nsym}", file=sys.stderr)
        return 2
    if args.timeout < 0:
        print("timeout must be >= 0", file=sys.stderr)
        return 2
    validate_kn(args.k, args.n)
    try:
        dest = parse_endpoint(args.to_ep)
    except argparse.ArgumentTypeError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    if args.encode:
        run_encode(args, dest)
    else:
        run_decode(args, dest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
