#!/usr/bin/env python3
"""WT32-ETH01 WInject bandwidth test.

Paths (pick one):
  --udp     winject-manager UDP (manager stamps MPDUs; scripts/manager_bw_test.sh)
  --tcp     winject-manager TCP ARQ (scripts/manager_bw_test.sh --tcp)
  --direct  host stamps MPDUs and injects to radios (scripts/stand_alone_test.py)

Sets CCA / channel / modulation over the UDP console unless --skip-config.
Peer air RX tests: use OFDM_24M on both radios (not HT MCS); see docs/winject.md.
"""

from __future__ import annotations

import argparse
import atexit
import socket
import struct
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

import mpdu

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

CONSOLE_PORT = 22
INJECT_PORT = 9000
HOST_PORT_A = 9001  # B→A: radio A forward / manager A demux listen
HOST_PORT_B = 9002  # A→B: radio B forward / manager B demux listen
DEFAULT_DOMAIN = "1234"
BUS_AB = "b2"  # A injects, B filters (manager stamps)
BUS_BA = "a1"  # B injects, A filters
CHANNEL_MIN = 1
CHANNEL_MAX = 14  # matches firmware WIFI_CHANNEL_*; 14 is 802.11b-only
TCP_SEND_A = 29000  # manager A TCP_SERVER (host sends A->B)
TCP_SEND_B = 29001  # manager B TCP_SERVER (host sends B->A)
# Manager gci: bind console_out, send gci to console_in (scripts/manager_bw_test.sh).
MGR_GCI_A_BIND = ("127.0.0.1", 2401)
MGR_GCI_A_DEST = ("127.0.0.1", 2400)
MGR_GCI_B_BIND = ("127.0.0.1", 2411)
MGR_GCI_B_DEST = ("127.0.0.1", 2410)
# Manager ARQ can stop reading when the window fills and reverse ACKs starve;
# without a send timeout, sendall blocks forever and phase() never returns.
TCP_SEND_TIMEOUT_S = 3.0
LOSS_WINDOW_S = 1.0
MAX_PAYLOAD = 1476

# Named PHY rates from docs/winject.md (20 MHz column for MCS).
PHY_KBPS = {
    "DSS_1M_L": 1000,
    "DSS_2M_S": 2000,
    "DSS_2M_L": 2000,
    "CCK_5M_L": 5500,
    "CCK_5M_S": 5500,
    "CCK_11M_L": 11000,
    "CCK_11M_S": 11000,
    "OFDM_6M": 6000,
    "OFDM_9M": 9000,
    "OFDM_12M": 12000,
    "OFDM_18M": 18000,
    "OFDM_24M": 24000,
    "OFDM_36M": 36000,
    "OFDM_48M": 48000,
    "OFDM_54M": 54000,
    "OFDM_MCS0_LGI": 6500,
    "OFDM_MCS1_LGI": 13000,
    "OFDM_MCS2_LGI": 19500,
    "OFDM_MCS3_LGI": 26000,
    "OFDM_MCS4_LGI": 39000,
    "OFDM_MCS5_LGI": 52000,
    "OFDM_MCS6_LGI": 58500,
    "OFDM_MCS7_LGI": 65000,
    "OFDM_MCS0_SGI": 7200,
    "OFDM_MCS1_SGI": 14400,
    "OFDM_MCS2_SGI": 21700,
    "OFDM_MCS3_SGI": 28900,
    "OFDM_MCS4_SGI": 43300,
    "OFDM_MCS5_SGI": 57800,
    "OFDM_MCS6_SGI": 65000,
    "OFDM_MCS7_SGI": 72200,
}

MODULATIONS = tuple(PHY_KBPS.keys())
# Channel 14 (2484 MHz) accepts DSSS/CCK only — same rule as firmware.
MODULATIONS_11B = tuple(
    name for name in MODULATIONS if name.startswith(("DSS_", "CCK_"))
)


def is_11b_modulation(name: str) -> bool:
    return name.upper().startswith(("DSS_", "CCK_"))


def channel_ok(channel: int) -> bool:
    return CHANNEL_MIN <= channel <= CHANNEL_MAX


def modulation_ok_for_channel(modulation: str, channel: int | None) -> bool:
    if channel != 14:
        return True
    return is_11b_modulation(modulation)


@dataclass
class RecvStats:
    packets: int = 0
    nbytes: int = 0
    first: float | None = None
    last: float | None = None
    seqs: set[int] = field(default_factory=set)
    dup_packets: int = 0

    def clear(self) -> None:
        self.packets = 0
        self.nbytes = 0
        self.first = None
        self.last = None
        self.seqs.clear()
        self.dup_packets = 0

    def copy(self) -> RecvStats:
        return RecvStats(
            packets=self.packets,
            nbytes=self.nbytes,
            first=self.first,
            last=self.last,
            seqs=set(self.seqs),
            dup_packets=self.dup_packets,
        )


@dataclass
class PhaseResult:
    sent: int = 0
    recv: int = 0
    kbps: float = 0.0

    @property
    def loss(self) -> float:
        if self.sent <= 0:
            return 100.0
        return max(0.0, 100.0 * (self.sent - self.recv) / self.sent)


@dataclass
class ModResult:
    modulation: str
    channel: int | None
    offer: float
    config_ok: bool = True
    integrity_ab: int = 0
    integrity_ba: int = 0
    integrity_ok: bool = False
    uni_ab: PhaseResult = field(default_factory=PhaseResult)
    uni_ba: PhaseResult = field(default_factory=PhaseResult)
    bidir_ab: PhaseResult = field(default_factory=PhaseResult)
    bidir_ba: PhaseResult = field(default_factory=PhaseResult)
    uni_ok: bool = False
    bidir_ok: bool = False
    overall: bool = False
    note: str = ""


class TestInterrupted(Exception):
    """Ctrl+C during a bandwidth phase; results use the elapsed send window."""

    def __init__(self, results: list[PhaseResult]) -> None:
        super().__init__("interrupted")
        self.results = results


def detect_host(peer: str) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((peer, CONSOLE_PORT))
        return sock.getsockname()[0]
    finally:
        sock.close()


# Per-command console retries (UDP loss / radio busy during PHY apply).
CONSOLE_CMD_RETRIES = 3
CONSOLE_CMD_TIMEOUT_S = 3.0


def console(
    ip: str,
    cmds: list[str],
    timeout: float = CONSOLE_CMD_TIMEOUT_S,
    quiet: bool = False,
) -> list[str]:
    last_err: Exception | None = None
    for attempt in range(3):
        try:
            return _console_once(ip, cmds, timeout, quiet)
        except OSError as err:
            last_err = err
            time.sleep(1.0 + attempt)
    raise last_err  # type: ignore[misc]


def _drain_console(sock: socket.socket) -> None:
    """Drop any queued datagrams so a retry does not consume a stale reply."""
    sock.settimeout(0.0)
    while True:
        try:
            sock.recvfrom(16384)
        except BlockingIOError:
            break
        except OSError:
            break


def _recv_reply(sock: socket.socket, timeout: float, *, accept_pong: bool = False) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            chunk, _ = sock.recvfrom(16384)
        except socket.timeout:
            continue
        if not chunk:
            continue
        if not chunk.endswith(b"\n"):
            chunk += b"\n"
        if chunk.strip() == b"pong":
            if accept_pong:
                return chunk
            continue
        return chunk
    raise TimeoutError("console read timeout")


def _console_once(ip: str, cmds: list[str], timeout: float, quiet: bool) -> list[str]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (ip, CONSOLE_PORT)
    replies: list[str] = []
    try:
        for cmd in cmds:
            wire = (cmd + "\n").encode()
            accept_pong = cmd.strip().split(None, 1)[0] == "ping"
            buf = b"nok timeout\n"
            for attempt in range(CONSOLE_CMD_RETRIES):
                _drain_console(sock)
                sock.sendto(wire, dest)
                try:
                    buf = _recv_reply(sock, timeout, accept_pong=accept_pong)
                    break
                except TimeoutError:
                    if attempt + 1 < CONSOLE_CMD_RETRIES:
                        if not quiet:
                            print(
                                f"[{ip}] {cmd}  timeout, "
                                f"retry {attempt + 1}/{CONSOLE_CMD_RETRIES - 1}"
                            )
                        time.sleep(0.2 * (attempt + 1))
                        continue
                    buf = b"nok timeout\n"
            text = buf.decode("utf-8", "replace")
            # ping -> pong is success (not an ok/nok line).
            if accept_pong and text.strip() == "pong":
                text = "ok\n"
            replies.append(text)
            if not quiet:
                print(f"[{ip}] {cmd}")
                if text.strip():
                    print(text.rstrip())
            else:
                status = (
                    "ok" if "nok " not in text else text.strip().splitlines()[-1]
                )
                print(f"[{ip}] {cmd}  {status}")
    finally:
        sock.close()
    return replies


def replies_ok(replies: list[str]) -> bool:
    return all("nok " not in text for text in replies)


def fmt_bus(bus: str) -> str:
    text = bus.strip().lower()
    if text.startswith("bus="):
        text = text[4:]
    if text.startswith("0x"):
        text = text[2:]
    if not 1 <= len(text) <= 2:
        raise SystemExit(f"invalid bus {bus!r}; want 1-2 hex digits")
    try:
        value = int(text, 16)
    except ValueError as err:
        raise SystemExit(f"invalid bus {bus!r}") from err
    if not 1 <= value <= 255:
        raise SystemExit(f"bus must be 1..ff (not broadcast 0): {bus}")
    return f"{value:x}"


def fmt_domain(value: int | str) -> str:
    if isinstance(value, int):
        n = value
    else:
        text = value.strip().lower()
        if text.startswith("0x"):
            text = text[2:]
        try:
            n = int(text, 16)
        except ValueError as err:
            raise SystemExit(f"invalid domain {value!r}") from err
    if not 1 <= n <= 65535:
        raise SystemExit(f"domain must be 1..ffff, got {value}")
    return f"{n:04x}"


def upstream_bind_cmds(
    host: str,
    inject_port: int,
    host_port: int,
    bus_tx: str,
    bus_rx: str,
) -> list[str]:
    # bus_tx / bus_rx are stamped by the manager; the radio only has a
    # single inject/forward pair.
    del bus_tx, bus_rx
    return [
        "unset_upstream_tx",
        "unset_upstream_rx",
        f"set_upstream_tx port={inject_port}",
        f"set_upstream_rx host={host} port={host_port}",
    ]


def upstream_config_cmds(
    host: str,
    inject_port: int,
    host_port: int,
    bus_tx: str,
    bus_rx: str,
    domain: str,
    extra_cmds: list[str] | None = None,
) -> list[str]:
    # Radio RX filters Addr3 by local mode prefix; must match host/manager stamp.
    cmds: list[str] = [
        "set_mode STANDALONE",
        f"set_domain {domain}",
    ]
    cmds.extend(upstream_bind_cmds(host, inject_port, host_port, bus_tx, bus_rx))
    if extra_cmds:
        cmds.extend(extra_cmds)
    return cmds


def configure_upstream(
    ip: str,
    host: str,
    *,
    inject_port: int,
    host_port: int,
    bus_tx: str,
    bus_rx: str,
    domain: str,
    quiet: bool,
    extra_cmds: list[str] | None = None,
) -> bool:
    replies = console(
        ip,
        upstream_config_cmds(
            host,
            inject_port,
            host_port,
            bus_tx,
            bus_rx,
            domain,
            extra_cmds=extra_cmds,
        ),
        quiet=quiet,
    )
    return replies_ok(replies)


def parse_status_field(text: str, key: str) -> str | None:
    prefix = f"{key}="
    for line in text.splitlines():
        parts = line.split()
        for i, part in enumerate(parts):
            if part.lower().startswith(prefix.lower()):
                return part.split("=", 1)[1] or None
            if part.lower() == key.lower() and i + 1 < len(parts):
                return parts[i + 1]
    return None


def parse_status_channel(text: str) -> int | None:
    raw = parse_status_field(text, "channel")
    if raw is not None and raw.isdigit():
        return int(raw)
    return None


def parse_status_modulation(text: str) -> str | None:
    raw = parse_status_field(text, "modulation")
    return raw.upper() if raw else None


def parse_status_domain(text: str) -> str | None:
    raw = parse_status_field(text, "domain")
    if raw:
        return None if raw.lower() == "unset" else raw.upper()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].lower() == "domain":
            return None if parts[1].lower() == "unset" else parts[1].upper()
    return None


def parse_status_mode(text: str) -> str | None:
    for line in text.splitlines():
        if not (line.startswith("device ") or line.startswith("status ")):
            continue
        for part in line.split():
            if part.lower().startswith("mode="):
                return part.split("=", 1)[1].upper() or None
        return None
    for part in text.split():
        if part.lower().startswith("mode="):
            return part.split("=", 1)[1].upper() or None
    return None


def log_status(ip: str, label: str) -> str:
    print(f"\n=== status {label} {ip} ===")
    try:
        return console(ip, ["status"], quiet=False)[0]
    except OSError as err:
        print(f"status failed: {err}")
        return ""


def configure_radio(
    ip: str,
    channel: int | None,
    modulation: str | None,
    cca: bool | None,
    quiet: bool,
) -> bool:
    # Channel 14 is 802.11b-only: apply DSSS/CCK before switching to 14,
    # and leave 14 before applying OFDM (matches firmware settings::apply_snapshot).
    # Radios default to BFC_TUNNEL_DEVICE; STANDALONE matches manager Addr3 stamp.
    # Channel / modulation / CCA are only sent when the caller asked for them.
    cmds: list[str] = ["set_mode STANDALONE"]
    if channel == 14:
        if modulation is not None:
            cmds.append(f"set_modulation {modulation}")
        cmds.append(f"set_channel {channel}")
    else:
        if channel is not None:
            cmds.append(f"set_channel {channel}")
        if modulation is not None:
            cmds.append(f"set_modulation {modulation}")
    if cca is not None:
        cmds.append(f"set_cca_enabled {1 if cca else 0}")
    replies = console(ip, cmds, quiet=quiet)
    return replies_ok(replies)


def make_payload(tag: bytes, seq: int, size: int) -> bytes:
    header = tag + b" " + f"{seq:08d}".encode() + b" "
    if len(header) >= size:
        return header[:size]
    return header + bytes(size - len(header))


def parse_seq(data: bytes) -> int | None:
    parts = data.split()
    if len(parts) < 2:
        return None
    try:
        return int(parts[1])
    except ValueError:
        return None


def apply_recv_payload(st: RecvStats, payload: bytes, now: float) -> None:
    """Count first delivery per sequence; ignore duplicate replays (MPDU/air/manager)."""
    seq = parse_seq(payload)
    if seq is not None:
        if seq in st.seqs:
            st.dup_packets += 1
            return
        st.seqs.add(seq)
    if st.first is None:
        st.first = now
    st.last = now
    st.packets += 1
    st.nbytes += len(payload)


def recv_delivered(stats: RecvStats) -> int:
    if stats.seqs:
        return len(stats.seqs)
    return stats.packets


class Listener:
    """UDP receiver. For standalone air path, payloads are full MPDUs; filter by bus."""

    def __init__(
        self, bind_ip: str, port: int, bus_filter: int | None = None
    ) -> None:
        self.stats = RecvStats()
        self.bus_filter = bus_filter
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self._sock.bind((bind_ip, port))
        self._sock.settimeout(0.1)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sock.close()

    def _note(self, payload: bytes) -> None:
        apply_recv_payload(self.stats, payload, time.monotonic())

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data, _addr = self._sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if self.bus_filter is None:
                self._note(data)
                continue
            for bus, body in mpdu.unpack_mpdu(data):
                if bus != self.bus_filter:
                    continue
                self._note(body)


def send_exact(
    dest: tuple[str, int],
    tag: bytes,
    count: int,
    size: int,
    gap: float,
    *,
    bus: int | None = None,
    domain: int | None = None,
) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for seq in range(count):
            payload = make_payload(tag, seq, size)
            if bus is not None and domain is not None:
                payload = mpdu.build_mpdu(
                    payload, bus, domain, mode="STANDALONE"
                )
            sock.sendto(payload, dest)
            if gap > 0:
                time.sleep(gap)
    finally:
        sock.close()
    return count


def send_window(
    dest: tuple[str, int],
    tag: bytes,
    size: int,
    duration: float,
    kbps: float,
    start: float,
    progress: list[int] | None = None,
    stop: threading.Event | None = None,
    *,
    bus: int | None = None,
    domain: int | None = None,
) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    seq = 0
    interval = (size * 8.0) / (kbps * 1000.0) if kbps > 0 else 0.0
    next_t = start
    try:
        while (stop is None or not stop.is_set()) and time.monotonic() - start < duration:
            now = time.monotonic()
            if interval > 0 and now < next_t:
                time.sleep(min(0.001, next_t - now))
                continue
            payload = make_payload(tag, seq, size)
            if bus is not None and domain is not None:
                payload = mpdu.build_mpdu(
                    payload, bus, domain, mode="STANDALONE"
                )
            sock.sendto(payload, dest)
            sent += 1
            if progress is not None:
                progress[0] = sent
            seq += 1
            if interval > 0:
                next_t += interval
                if next_t < time.monotonic() - interval:
                    next_t = time.monotonic()
    finally:
        sock.close()
    return sent


def send_tcp_record(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(struct.pack("!H", len(payload)) + payload)


class TcpSendStall(OSError):
    """Host TCP send blocked longer than TCP_SEND_TIMEOUT_S (ARQ backpressure)."""

    def __init__(self, message: str, sent: int = 0) -> None:
        super().__init__(message)
        self.sent = sent


def tcp_connect(host: str, port: int, timeout: float = 5.0) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout)
    # Bound send so ARQ stalls surface as TcpSendStall instead of a hang.
    sock.settimeout(TCP_SEND_TIMEOUT_S)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def close_quiet(sock: socket.socket | None) -> None:
    if sock is None:
        return
    try:
        sock.close()
    except OSError:
        pass


class TcpListener:
    """TCP server; records are uint16 BE length + payload (manager TCP path)."""

    def __init__(self, bind_ip: str, port: int) -> None:
        self.stats = RecvStats()
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
            data = bytes(buf[2:2 + plen])
            del buf[:2 + plen]
            apply_recv_payload(self.stats, data, time.monotonic())

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


def send_exact_tcp(
    dest: tuple[str, int], tag: bytes, count: int, size: int, gap: float,
    sock: socket.socket | None = None,
) -> int:
    own = sock is None
    if own:
        sock = tcp_connect(dest[0], dest[1])
    assert sock is not None
    sent = 0
    try:
        for seq in range(count):
            try:
                send_tcp_record(sock, make_payload(tag, seq, size))
            except (TimeoutError, socket.timeout, ConnectionResetError,
                    BrokenPipeError, ConnectionError) as err:
                raise TcpSendStall(
                    f"tcp send stall to {dest[0]}:{dest[1]} after {sent}/{count}",
                    sent=sent,
                ) from err
            sent += 1
            if gap > 0:
                time.sleep(gap)
    finally:
        if own:
            close_quiet(sock)
    return sent


def send_window_tcp(
    dest: tuple[str, int],
    tag: bytes,
    size: int,
    duration: float,
    kbps: float,
    start: float,
    sock: socket.socket | None = None,
    progress: list[int] | None = None,
    stop: threading.Event | None = None,
) -> int:
    own = sock is None
    if own:
        sock = tcp_connect(dest[0], dest[1])
    assert sock is not None
    sent = 0
    seq = 0
    interval = (size * 8.0) / (kbps * 1000.0) if kbps > 0 else 0.0
    next_t = start
    try:
        while (stop is None or not stop.is_set()) and time.monotonic() - start < duration:
            now = time.monotonic()
            if interval > 0 and now < next_t:
                time.sleep(min(0.001, next_t - now))
                continue
            try:
                send_tcp_record(sock, make_payload(tag, seq, size))
            except (TimeoutError, socket.timeout, ConnectionResetError,
                    BrokenPipeError, ConnectionError, OSError) as err:
                if stop is not None and stop.is_set():
                    return sent
                raise TcpSendStall(
                    f"tcp send stall to {dest[0]}:{dest[1]} after {sent} pkts "
                    f"(ARQ window full / ACKs starved?)",
                    sent=sent,
                ) from err
            sent += 1
            if progress is not None:
                progress[0] = sent
            seq += 1
            if interval > 0:
                next_t += interval
                if next_t < time.monotonic() - interval:
                    next_t = time.monotonic()
    finally:
        if own:
            close_quiet(sock)
    return sent


def auto_offer_kbps(modulation: str, size: int) -> float:
    """Payload offer slightly under estimated 802.11 goodput for this PHY."""
    phy = PHY_KBPS.get(modulation.upper(), 1000)
    preamble_us = 200.0 if phy <= 11000 else 40.0
    mac_us = 400.0 if phy <= 11000 else 150.0
    mpdu_bits = (24 + size) * 8
    air_us = preamble_us + (mpdu_bits / phy) * 1000.0 + mac_us
    raw = (size * 8.0) / (air_us / 1000.0)
    # Legacy OFDM_24M: allow >15 Mbps payload targets when air loss is low.
    if phy >= 24000:
        return min(phy * 0.78, raw * 0.98)
    return raw * 0.85


def goodput_kbps(stats: RecvStats, duration: float) -> float:
    if duration <= 0:
        return 0.0
    return (stats.nbytes * 8.0) / duration / 1000.0


def live_kbps(nbytes: int, dt: float) -> float:
    if dt <= 0.0 or nbytes <= 0:
        return 0.0
    return (nbytes * 8.0) / dt / 1000.0


def fmt_loss_pct(sent: int, recv: int) -> str:
    if sent <= 0:
        return "n/a"
    return f"{max(0.0, 100.0 * (sent - recv) / sent):.1f}%"


def push_window_loss(
    hist: deque[tuple[float, int, int]],
    now: float,
    sent: int,
    recv: int,
    window_s: float = LOSS_WINDOW_S,
) -> str:
    """Packet loss over the last window_s seconds (keeps one sample at/before cutoff)."""
    hist.append((now, sent, recv))
    cutoff = now - window_s
    while len(hist) > 1 and hist[1][0] <= cutoff:
        hist.popleft()
    _t0, sent0, recv0 = hist[0]
    return fmt_loss_pct(sent - sent0, recv - recv0)


def to_phase(sent: int, stats: RecvStats, duration: float) -> PhaseResult:
    return PhaseResult(
        sent=sent, recv=recv_delivered(stats), kbps=goodput_kbps(stats, duration)
    )


def loss_ok(phase: PhaseResult, limit: float, paced: bool) -> bool:
    if phase.recv <= 0 or phase.sent <= 0:
        return False
    if not paced:
        return True
    return phase.loss <= limit


def parse_modulations(text: str) -> list[str]:
    raw = text.strip()
    if raw.lower() == "all":
        raise SystemExit("use --all to sweep every modulation")
    names = [part.strip() for part in raw.split(",") if part.strip()]
    if not names:
        raise SystemExit("no modulations given")
    unknown = [name for name in names if name.upper() not in PHY_KBPS]
    if unknown:
        raise SystemExit(
            "unknown modulation: "
            + ", ".join(unknown)
            + "\nknown: "
            + " ".join(MODULATIONS)
        )
    return [name.upper() for name in names]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="WT32-ETH01 WInject bandwidth test (--udp/--tcp managers, --direct radios)"
    )
    p.add_argument("--a", default="192.168.253.11", help="radio A Ethernet IP")
    p.add_argument("--b", default="192.168.253.12", help="radio B Ethernet IP")
    p.add_argument(
        "--host",
        default="",
        help="host IP used when probing radio status (default: auto)",
    )
    p.add_argument(
        "--domain",
        default=DEFAULT_DOMAIN,
        help=f"shared air domain, hex 1..ffff (default {DEFAULT_DOMAIN})",
    )
    p.add_argument(
        "--bus-ab",
        default=BUS_AB,
        help=f"bus A injects / B filters (default {BUS_AB}; informational)",
    )
    p.add_argument(
        "--bus-ba",
        default=BUS_BA,
        help=f"bus B injects / A filters (default {BUS_BA}; informational)",
    )
    p.add_argument(
        "--channel",
        type=int,
        default=None,
        help=f"set_channel on both radios ({CHANNEL_MIN}-{CHANNEL_MAX}; "
        "14 is 802.11b-only); omit to keep existing",
    )
    p.add_argument(
        "--modulation",
        default=None,
        help="one name or comma-separated list; omit to keep existing",
    )
    p.add_argument(
        "--all",
        action="store_true",
        help="sweep every firmware modulation",
    )
    p.add_argument("--size", type=int, default=1400, help="payload bytes (max 1476)")
    p.add_argument("--duration", type=float, default=5.0, help="seconds per bandwidth phase")
    p.add_argument(
        "--drain",
        type=float,
        default=1.0,
        help=(
            "after send window: wait up to this many seconds for recv==sent, "
            "then settle the same duration (0 = no drain)"
        ),
    )
    p.add_argument(
        "--kbps",
        type=float,
        default=-1.0,
        help="paced payload offer in kbit/s (-1 = auto from modulation, 0 = flood)",
    )
    p.add_argument("--integrity", type=int, default=20, help="integrity packets per direction")
    p.add_argument(
        "--cca",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="set_cca_enabled on both radios; omit to keep existing",
    )
    p.add_argument("--skip-config", action="store_true", help="do not touch the UDP console")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument(
        "--udp",
        action="store_true",
        help="winject-manager UDP ports (datagrams; manager stamps MPDUs)",
    )
    mode.add_argument(
        "--tcp",
        action="store_true",
        help="winject-manager TCP ports (length-prefixed ARQ)",
    )
    mode.add_argument(
        "--direct",
        action="store_true",
        help="stamp MPDUs on host and inject to radios (no manager)",
    )
    p.add_argument(
        "--skip-upstream",
        action="store_true",
        help="--direct only: do not rebind sut/sur",
    )
    p.add_argument(
        "--tcp-send-a",
        type=int,
        default=TCP_SEND_A,
        help=f"manager A send port for A->B (default {TCP_SEND_A})",
    )
    p.add_argument(
        "--tcp-send-b",
        type=int,
        default=TCP_SEND_B,
        help=f"manager B send port for B->A (default {TCP_SEND_B})",
    )
    p.add_argument(
        "--test-integ",
        action="store_true",
        help="run integrity check (both directions)",
    )
    p.add_argument(
        "--test-ab",
        action="store_true",
        help="run unidirectional A->B bandwidth phase",
    )
    p.add_argument(
        "--test-ba",
        action="store_true",
        help="run unidirectional B->A bandwidth phase",
    )
    p.add_argument(
        "--test-bidir",
        action="store_true",
        help="run simultaneous A+B bandwidth phase",
    )
    p.add_argument("--verbose", action="store_true", help="print full console replies")
    p.add_argument(
        "--drop-stages",
        action="store_true",
        help="print per-stage drop counts after each unidirectional phase (manager gci + radio status)",
    )
    return p.parse_args()



def fmt_summary(
    results: list[ModResult],
    paced: bool,
    *,
    integ: bool = True,
    ab: bool = True,
    ba: bool = True,
    bidir: bool = False,
) -> str:
    header = "| modulation      | ch | offer kbps |"
    sep = "|-----------------|----|------------|"
    if integ:
        header += "  int A->B |  int B->A |"
        sep += "-----------|-----------|"
    if ab:
        header += " A->B kbps | A->B loss |"
        sep += "-----------|-----------|"
    if ba:
        header += " B->A kbps | B->A loss |"
        sep += "-----------|-----------|"
    if bidir:
        header += " A+B A kbps | A+B B kbps | A+B A loss | A+B B loss |"
        sep += "------------|------------|------------|------------|"
    lines = [header, sep]
    for r in results:
        int_ab = f"{r.integrity_ab}" if r.config_ok else "-"
        int_ba = f"{r.integrity_ba}" if r.config_ok else "-"

        def cell(phase: PhaseResult) -> tuple[str, str]:
            if not r.config_ok or phase.sent == 0:
                return "-", "-"
            loss = f"{phase.loss:.1f}" if paced else "n/a"
            return f"{phase.kbps:.1f}", loss

        ch = "-" if r.channel is None else r.channel
        line = f"| {r.modulation:<15} | {ch:>2} | {r.offer:>10.0f} |"
        if integ:
            line += f" {int_ab:>9} | {int_ba:>9} |"
        if ab:
            ab_k, ab_l = cell(r.uni_ab)
            line += f" {ab_k:>9} | {ab_l:>9} |"
        if ba:
            ba_k, ba_l = cell(r.uni_ba)
            line += f" {ba_k:>9} | {ba_l:>9} |"
        if bidir:
            a_k, a_l = cell(r.bidir_ab)
            b_k, b_l = cell(r.bidir_ba)
            line += f" {a_k:>10} | {b_k:>10} | {a_l:>10} | {b_l:>10} |"
        lines.append(line)
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    if not (args.test_integ or args.test_ab or args.test_ba or args.test_bidir):
        args.test_integ = True
        args.test_ab = True
        args.test_ba = True
    if not (args.udp or args.tcp or args.direct):
        raise SystemExit("pick a path: --udp, --tcp, or --direct (see scripts/manager_bw_test.sh)")
    skip_phy = args.skip_config
    skip_upstream = (not args.direct) or args.skip_upstream or args.skip_config
    if args.size < 16 or args.size > MAX_PAYLOAD:
        raise SystemExit(f"--size must be 16..{MAX_PAYLOAD}")
    if args.channel is not None and not channel_ok(args.channel):
        raise SystemExit(f"--channel must be {CHANNEL_MIN}-{CHANNEL_MAX}")
    if args.all and args.modulation is not None:
        raise SystemExit("use --all or --modulation, not both")

    apply_modulation = args.all or args.modulation is not None
    if args.all:
        mods = list(MODULATIONS_11B if args.channel == 14 else MODULATIONS)
    elif args.modulation is not None:
        mods = parse_modulations(args.modulation)
        bad = [m for m in mods if not modulation_ok_for_channel(m, args.channel)]
        if bad:
            raise SystemExit(
                f"channel 14 rejects OFDM/MCS; not allowed: {', '.join(bad)}"
            )
    else:
        mods = []

    host = args.host or detect_host(args.a)
    quiet = not args.verbose
    if args.cca is None:
        cca_label = "unchanged"
    else:
        cca_label = "enabled" if args.cca else "disabled"
    domain = fmt_domain(args.domain)
    bus_ab = fmt_bus(args.bus_ab)
    bus_ba = fmt_bus(args.bus_ba)
    if bus_ab == bus_ba:
        raise SystemExit("--bus-ab and --bus-ba must differ")
    print(f"host {host}")
    print(f"domain {domain}  bus A→B {bus_ab}  bus B→A {bus_ba}")

    status_a = log_status(args.a, "A")
    status_b = log_status(args.b, "B")
    display_channel: int | None = args.channel
    if args.channel is None:
        ch_a = parse_status_channel(status_a)
        ch_b = parse_status_channel(status_b)
        display_channel = ch_a
        if ch_a is not None and ch_b is not None and ch_a != ch_b:
            print(f"warning: A is on channel {ch_a}, B is on channel {ch_b}")

    if not apply_modulation:
        mod_a = parse_status_modulation(status_a)
        mod_b = parse_status_modulation(status_b)
        if mod_a is not None and mod_b is not None and mod_a != mod_b:
            print(f"warning: A is {mod_a}, B is {mod_b}")
        current = mod_a or mod_b
        if current is None:
            raise SystemExit("could not read existing modulation; pass --modulation or --all")
        if not modulation_ok_for_channel(current, args.channel):
            raise SystemExit(
                f"channel 14 rejects OFDM/MCS; radios are {current} — "
                "pass --modulation DSS_1M_L (or another DSSS/CCK rate)"
            )
        mods = [current]

    for label, text in (("A", status_a), ("B", status_b)):
        mode = parse_status_mode(text)
        if mode and mode != "STANDALONE":
            print(f"warning: {label} mode is {mode} (want STANDALONE Addr3)")
    dom_a = parse_status_domain(status_a)
    dom_b = parse_status_domain(status_b)
    if not dom_a or dom_a == "UNSET" or not dom_b or dom_b == "UNSET":
        print("warning: domain unset on one or both radios")
    elif dom_a != dom_b:
        print(f"warning: A domain {dom_a}, B domain {dom_b}")
    elif dom_a != domain.upper():
        print(f"warning: radios domain {dom_a}, test --domain {domain}")

    channel_label = str(display_channel) if display_channel is not None else "existing"
    if args.channel is None:
        channel_label += " (unchanged)"
    mod_label_suffix = " (unchanged)" if not apply_modulation else ""
    print(f"\nA {args.a}  B {args.b}  channel {channel_label}  cca {cca_label}")
    print(f"modulations ({len(mods)}): {' '.join(mods)}{mod_label_suffix}")
    print(
        f"payload {args.size} B  duration {args.duration}s  drain "
        f"{'off' if args.drain <= 0 else f'wait≤{args.drain:g}s then settle {args.drain:g}s'}"
    )
    bus_ab_i = mpdu.parse_bus_int(bus_ab)
    bus_ba_i = mpdu.parse_bus_int(bus_ba)
    domain_i = mpdu.parse_domain_int(domain)

    if args.tcp:
        print(
            f"tcp managers: send A->B {args.tcp_send_a}  send B->A {args.tcp_send_b}; "
            f"listen B->A {HOST_PORT_A}  listen A->B {HOST_PORT_B}"
        )
        listen_a = TcpListener("127.0.0.1", HOST_PORT_A)
        listen_b = TcpListener("127.0.0.1", HOST_PORT_B)
        dest_a = ("127.0.0.1", args.tcp_send_a)
        dest_b = ("127.0.0.1", args.tcp_send_b)
    elif args.udp:
        print(
            f"udp managers: send A->B {args.tcp_send_a}  send B->A {args.tcp_send_b}; "
            f"listen B->A {HOST_PORT_A}  listen A->B {HOST_PORT_B}"
        )
        listen_a = Listener("127.0.0.1", HOST_PORT_A)
        listen_b = Listener("127.0.0.1", HOST_PORT_B)
        dest_a = ("127.0.0.1", args.tcp_send_a)
        dest_b = ("127.0.0.1", args.tcp_send_b)
    else:
        print(
            f"direct MPDU inject {INJECT_PORT}; forward host "
            f"{HOST_PORT_A}/{HOST_PORT_B} (STANDALONE domain {domain})"
        )
        if not skip_upstream:
            print("\n=== configure upstream ===")
            if not configure_upstream(
                args.a,
                host,
                inject_port=INJECT_PORT,
                host_port=HOST_PORT_A,
                bus_tx=bus_ab,
                bus_rx=bus_ba,
                domain=domain,
                quiet=quiet,
            ):
                raise SystemExit("failed to configure upstream on A")
            if not configure_upstream(
                args.b,
                host,
                inject_port=INJECT_PORT,
                host_port=HOST_PORT_B,
                bus_tx=bus_ba,
                bus_rx=bus_ab,
                domain=domain,
                quiet=quiet,
            ):
                raise SystemExit("failed to configure upstream on B")
        listen_a = Listener(host, HOST_PORT_A, bus_filter=bus_ba_i)
        listen_b = Listener(host, HOST_PORT_B, bus_filter=bus_ab_i)
        dest_a = (args.a, INJECT_PORT)
        dest_b = (args.b, INJECT_PORT)

    listen_a.start()
    listen_b.start()
    time.sleep(0.2)

    tcp_sock_a: socket.socket | None = None
    tcp_sock_b: socket.socket | None = None
    if args.tcp:
        tcp_sock_a = tcp_connect(dest_a[0], dest_a[1])
        time.sleep(0.3)
        tcp_sock_b = tcp_connect(dest_b[0], dest_b[1])
        time.sleep(0.3)
        atexit.register(lambda: close_quiet(tcp_sock_a))
        atexit.register(lambda: close_quiet(tcp_sock_b))

    def tcp_sock_for(dest: tuple[str, int]) -> socket.socket | None:
        return tcp_sock_a if dest == dest_a else tcp_sock_b

    def reconnect_tcp(dest: tuple[str, int]) -> socket.socket:
        nonlocal tcp_sock_a, tcp_sock_b
        if dest == dest_a:
            close_quiet(tcp_sock_a)
            tcp_sock_a = tcp_connect(dest_a[0], dest_a[1])
            return tcp_sock_a
        close_quiet(tcp_sock_b)
        tcp_sock_b = tcp_connect(dest_b[0], dest_b[1])
        return tcp_sock_b

    def bus_for(dest: tuple[str, int]) -> int:
        return bus_ab_i if dest == dest_a else bus_ba_i

    def send_exact_fn(dest, tag, count, size, gap):
        if args.tcp:
            sock = tcp_sock_for(dest)
            try:
                return send_exact_tcp(dest, tag, count, size, gap, sock)
            except TcpSendStall as err:
                print(f"warning: {err}; reconnecting")
                sock = reconnect_tcp(dest)
                return send_exact_tcp(dest, tag, count, size, gap, sock)
        if args.direct:
            return send_exact(
                dest, tag, count, size, gap, bus=bus_for(dest), domain=domain_i
            )
        return send_exact(dest, tag, count, size, gap)

    stop = threading.Event()

    def send_window_fn(dest, tag, size, duration, kbps, start, progress=None):
        if args.tcp:
            sock = tcp_sock_for(dest)
            try:
                return send_window_tcp(
                    dest, tag, size, duration, kbps, start, sock, progress, stop
                )
            except TcpSendStall:
                reconnect_tcp(dest)
                raise
        if args.direct:
            return send_window(
                dest,
                tag,
                size,
                duration,
                kbps,
                start,
                progress,
                stop,
                bus=bus_for(dest),
                domain=domain_i,
            )
        return send_window(dest, tag, size, duration, kbps, start, progress, stop)

    def reset() -> None:
        listen_a.stats.clear()
        listen_b.stats.clear()

    def run_integrity() -> tuple[int, int]:
        reset()
        send_exact_fn(dest_a, b"A", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ab = listen_b.stats.packets
        reset()
        send_exact_fn(dest_b, b"B", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ba = listen_a.stats.packets
        return ab, ba

    def phase(
        senders: list[tuple[tuple[str, int], bytes, Listener | TcpListener]],
        kbps: float,
    ) -> list[PhaseResult]:
        nonlocal tcp_sock_a, tcp_sock_b
        stop.clear()
        reset()
        start = time.monotonic()
        results = [0] * len(senders)
        stalls: list[TcpSendStall | None] = [None] * len(senders)
        threads: list[threading.Thread] = []
        sent_live = [[0] for _ in senders]
        join_timeout = args.duration + TCP_SEND_TIMEOUT_S + 2.0

        def dir_label(dest: tuple[str, int]) -> str:
            if dest == dest_a:
                return "A->B"
            if dest == dest_b:
                return "B->A"
            return f"{dest[0]}:{dest[1]}"

        def run_one(idx: int, dest: tuple[str, int], tag: bytes) -> None:
            try:
                results[idx] = send_window_fn(
                    dest, tag, args.size, args.duration, kbps, start, sent_live[idx]
                )
            except TcpSendStall as err:
                sent_live[idx][0] = err.sent
                results[idx] = err.sent
                stalls[idx] = err

        for i, (dest, tag, _lis) in enumerate(senders):
            t = threading.Thread(target=run_one, args=(i, dest, tag), daemon=True)
            threads.append(t)
            t.start()

        use_cr = sys.stdout.isatty()
        print_interval = 0.25 if use_cr else 1.0
        deadline = start + join_timeout
        prev_t = start
        prev_sent = [0] * len(senders)
        prev_nbytes = [0] * len(senders)
        last_print = start
        win_hist: list[deque[tuple[float, int, int]]] = [
            deque([(start, 0, 0)]) for _ in senders
        ]

        def progress_line() -> str:
            now = time.monotonic()
            elapsed = max(0.0, now - start)
            dt = max(now - prev_t, 1e-6)
            parts = [f"{elapsed:6.1f}/{args.duration:.0f}s"]
            for i, (dest, _tag, lis) in enumerate(senders):
                sent_n = sent_live[i][0]
                recv_n = recv_delivered(lis.stats)
                recv_b = lis.stats.nbytes
                send_rate = live_kbps((sent_n - prev_sent[i]) * args.size, dt)
                recv_rate = live_kbps(recv_b - prev_nbytes[i], dt)
                loss_rt = push_window_loss(win_hist[i], now, sent_n, recv_n)
                parts.append(
                    f"{send_rate: 8.1f} {dir_label(dest)} {recv_rate: 8.1f} "
                    f"loss_rt {loss_rt:>6}"
                )
            return "  ".join(parts)

        def emit_progress(line: str, *, newline: bool = False) -> None:
            if use_cr:
                end = "\n" if newline else ""
                print(f"\r{line:<220}", end=end, flush=True)
            else:
                print(line, flush=True)

        def halt_senders() -> None:
            nonlocal tcp_sock_a, tcp_sock_b
            stop.set()
            close_quiet(tcp_sock_a)
            close_quiet(tcp_sock_b)
            tcp_sock_a = None
            tcp_sock_b = None
            for t in threads:
                t.join(timeout=1.0)

        interrupted = False
        try:
            while True:
                alive = [t for t in threads if t.is_alive()]
                now = time.monotonic()
                due = now - last_print >= print_interval
                finished = not alive and now - last_print >= 0.05
                if due or finished:
                    emit_progress(progress_line())
                    last_print = now
                    prev_t = now
                    prev_sent = [cell[0] for cell in sent_live]
                    prev_nbytes = [lis.stats.nbytes for _dest, _tag, lis in senders]
                if not alive or now >= deadline:
                    break
                wait = min(print_interval, max(0.0, deadline - now))
                alive[0].join(timeout=wait)
        except KeyboardInterrupt:
            interrupted = True
            try:
                halt_senders()
            except KeyboardInterrupt:
                pass

        if use_cr:
            print(flush=True)

        def drain_line(elapsed: float, timeout: float) -> str:
            now = time.monotonic()
            parts = [f"drain {elapsed:4.1f}/{timeout:.0f}s"]
            for i, (dest, _tag, lis) in enumerate(senders):
                sent_n = results[i] if results[i] > 0 else sent_live[i][0]
                recv_n = recv_delivered(lis.stats)
                loss_rt = push_window_loss(win_hist[i], now, sent_n, recv_n)
                parts.append(
                    f"{dir_label(dest)} {recv_n}/{sent_n} "
                    f"loss_rt {loss_rt:>6}"
                )
            return "  ".join(parts)

        if interrupted:
            print("interrupted")
        else:
            for t in threads:
                if t.is_alive():
                    print(
                        f"warning: sender thread still blocked after {join_timeout:.1f}s; "
                        "closing TCP sockets"
                    )
                    close_quiet(tcp_sock_a)
                    close_quiet(tcp_sock_b)
                    tcp_sock_a = None
                    tcp_sock_b = None
                    t.join(timeout=TCP_SEND_TIMEOUT_S + 1.0)
            for err in stalls:
                if err is not None:
                    print(f"warning: {err}")
            if args.tcp and (tcp_sock_a is None or tcp_sock_b is None):
                if tcp_sock_a is None:
                    tcp_sock_a = tcp_connect(dest_a[0], dest_a[1])
                if tcp_sock_b is None:
                    tcp_sock_b = tcp_connect(dest_b[0], dest_b[1])
            # Wait up to --drain for recv==sent, then settle --drain more.
            # --drain 0 skips both. (TCP ARQ used to floor this at 10s.)
            if args.drain > 0:
                timeout = args.drain
                t0 = time.monotonic()
                deadline_d = t0 + timeout
                last_dprint = 0.0
                drained = False
                try:
                    while time.monotonic() < deadline_d:
                        drained = True
                        for i, (_dest, _tag, lis) in enumerate(senders):
                            sent_n = results[i] if results[i] > 0 else sent_live[i][0]
                            if sent_n > 0 and recv_delivered(lis.stats) < sent_n:
                                drained = False
                                break
                        if drained:
                            break
                        now = time.monotonic()
                        if now - last_dprint >= print_interval:
                            emit_progress(drain_line(now - t0, timeout))
                            last_dprint = now
                        time.sleep(0.05)
                except KeyboardInterrupt:
                    interrupted = True
                    if use_cr and last_dprint > 0.0:
                        print(flush=True)
                    print("interrupted")
                else:
                    if use_cr and last_dprint > 0.0:
                        emit_progress(
                            drain_line(time.monotonic() - t0, timeout), newline=True
                        )
                    if not drained:
                        for i, (dest, _tag, lis) in enumerate(senders):
                            sent_n = results[i] if results[i] > 0 else sent_live[i][0]
                            recv_n = recv_delivered(lis.stats)
                            if sent_n > 0 and recv_n < sent_n:
                                print(
                                    f"warning: {dir_label(dest)} drain incomplete "
                                    f"{recv_n}/{sent_n} after {timeout:.1f}s"
                                )
                if not interrupted:
                    try:
                        time.sleep(args.drain)
                    except KeyboardInterrupt:
                        interrupted = True
                        print("interrupted")

        elapsed = time.monotonic() - start
        if interrupted and elapsed < args.duration:
            measure_s = max(elapsed, 1e-6)
        else:
            measure_s = args.duration if args.duration > 0 else max(elapsed, 1e-6)
        out: list[PhaseResult] = []
        for i, (_dest, _tag, lis) in enumerate(senders):
            sent_n = results[i] if results[i] > 0 else sent_live[i][0]
            kbps = (lis.stats.nbytes * 8.0) / measure_s / 1000.0
            out.append(
                PhaseResult(sent=sent_n, recv=recv_delivered(lis.stats), kbps=kbps)
            )
        if interrupted:
            raise TestInterrupted(out)
        return out

    def direction_label(dest: tuple[str, int]) -> str:
        if dest == dest_a:
            return "A->B"
        if dest == dest_b:
            return "B->A"
        return f"{dest[0]}:{dest[1]}"

    def drop_path_meta(
        dest: tuple[str, int],
    ) -> tuple[str, str, tuple[tuple[str, int], tuple[str, int]], tuple[tuple[str, int], tuple[str, int]], int, int]:
        if dest == dest_a:
            return (
                args.a,
                args.b,
                (MGR_GCI_A_BIND, MGR_GCI_A_DEST),
                (MGR_GCI_B_BIND, MGR_GCI_B_DEST),
                0,
                0,
            )
        return (
            args.b,
            args.a,
            (MGR_GCI_B_BIND, MGR_GCI_B_DEST),
            (MGR_GCI_A_BIND, MGR_GCI_A_DEST),
            1,
            1,
        )

    def report_drop_stages(
        snap_before: object,
        dest: tuple[str, int],
        pr: PhaseResult,
    ) -> None:
        from drop_path_probe import capture_drop_path_snap, drop_path_delta, print_drop_path_stages

        tx_r, rx_r, mtx_p, mrx_p, stx, srx = drop_path_meta(dest)
        try:
            snap_after = capture_drop_path_snap(tx_r, rx_r, mtx_p, mrx_p, stx, srx)
            dp = drop_path_delta(snap_before, snap_after, pr.sent, pr.recv)
            print_drop_path_stages(direction_label(dest), dp)
        except OSError as err:
            print(f"warning: drop-stages snap end failed: {err}")

    def reset_radio_phase_stats(tx_radio: str, rx_radio: str) -> None:
        for ip in (tx_radio, rx_radio):
            try:
                console(
                    ip,
                    ["reset_channel_stats", "reset_promisc_stats"],
                    quiet=quiet,
                    timeout=3,
                )
            except OSError:
                pass

    def take_phase(
        senders: list[tuple[tuple[str, int], bytes, Listener | TcpListener]],
        kbps: float,
    ) -> tuple[list[PhaseResult], bool]:
        drop_before = None
        drop_dest: tuple[str, int] | None = None
        if args.drop_stages and not args.direct and len(senders) == 1:
            from drop_path_probe import capture_drop_path_snap

            drop_dest = senders[0][0]
            tx_r, rx_r, mtx_p, mrx_p, stx, srx = drop_path_meta(drop_dest)
            # Let wifi_tx / 802.11 completions drain after the prior leg.
            time.sleep(0.5)
            reset_radio_phase_stats(tx_r, rx_r)
            time.sleep(0.15)
            try:
                drop_before = capture_drop_path_snap(tx_r, rx_r, mtx_p, mrx_p, stx, srx)
            except OSError as err:
                print(f"warning: drop-stages snap start failed: {err}")
        try:
            results = phase(senders, kbps)
        except TestInterrupted as err:
            results = err.results
            if drop_before is not None and drop_dest is not None and results:
                report_drop_stages(drop_before, drop_dest, results[0])
            return results, True
        if drop_before is not None and drop_dest is not None and results:
            report_drop_stages(drop_before, drop_dest, results[0])
        return results, False

    def print_snap(
        prefix: str, pr: PhaseResult, stats: RecvStats | None = None
    ) -> None:
        line = (
            f"{prefix} sent {pr.sent} recv {pr.recv}  "
            f"{pr.kbps:.1f} kbps  loss {pr.loss:.1f}%"
        )
        if stats is not None and stats.dup_packets > 0:
            line += f"  dup_ignored {stats.dup_packets}"
        if pr.recv > pr.sent:
            line += "  warning: recv>sent (foreign/bus?)"
        print(line)

    all_results: list[ModResult] = []
    interrupted = False
    current: ModResult | None = None
    try:
        for idx, mod in enumerate(mods, 1):
            offer = args.kbps
            if offer < 0:
                offer = auto_offer_kbps(mod, args.size)
                if args.tcp:
                    # TCP ARQ needs reverse ACK capacity.
                    offer *= 0.55
            paced = offer > 0
            print(
                f"\n=== [{idx}/{len(mods)}] ch {channel_label}  {mod}{mod_label_suffix}  "
                f"cca {cca_label}  offer {offer:.0f} kbps ==="
            )
            result = ModResult(modulation=mod, channel=display_channel, offer=offer)
            current = result

            if not skip_phy:
                set_mod = mod if apply_modulation else None
                try:
                    ok_a = configure_radio(args.a, args.channel, set_mod, args.cca, quiet)
                    ok_b = configure_radio(args.b, args.channel, set_mod, args.cca, quiet)
                except OSError as err:
                    result.config_ok = False
                    result.note = "CONFIG"
                    result.overall = False
                    print(f"configure failed: {err}")
                    all_results.append(result)
                    current = None
                    continue
                if not ok_a or not ok_b:
                    result.config_ok = False
                    result.note = "CONFIG"
                    result.overall = False
                    print("radio config failed")
                    all_results.append(result)
                    current = None
                    continue
                if args.channel is not None or apply_modulation:
                    time.sleep(1.2)

            def mark_int() -> None:
                nonlocal interrupted
                interrupted = True
                result.note = "INT"
                result.overall = False
                print("INTERRUPTED")
                all_results.append(result)

            if args.test_integ:
                print("-- integrity")
                a_to_b, b_to_a = run_integrity()
                print(f"A -> B  {a_to_b}/{args.integrity}")
                print(f"B -> A  {b_to_a}/{args.integrity}")
                if a_to_b < args.integrity or b_to_a < args.integrity:
                    print("integrity retry")
                    time.sleep(0.5)
                    a_to_b, b_to_a = run_integrity()
                    print(f"A -> B  {a_to_b}/{args.integrity}")
                    print(f"B -> A  {b_to_a}/{args.integrity}")
                result.integrity_ab = a_to_b
                result.integrity_ba = b_to_a
                result.integrity_ok = (
                    a_to_b >= args.integrity and b_to_a >= args.integrity
                )
            else:
                result.integrity_ok = True

            loss_lim = 25.0
            result.uni_ok = True
            if args.test_ab or args.test_ba:
                print("-- unidirectional")
            if args.test_ab:
                snaps, stopped = take_phase([(dest_a, b"A", listen_b)], offer)
                result.uni_ab = snaps[0]
                print_snap("A->B", result.uni_ab, listen_b.stats)
                if stopped:
                    mark_int()
                    current = None
                    break
                result.uni_ok = result.uni_ok and loss_ok(result.uni_ab, loss_lim, paced)
            if args.test_ba:
                snaps, stopped = take_phase([(dest_b, b"B", listen_a)], offer)
                result.uni_ba = snaps[0]
                print_snap("B->A", result.uni_ba, listen_a.stats)
                if stopped:
                    mark_int()
                    current = None
                    break
                result.uni_ok = result.uni_ok and loss_ok(result.uni_ba, loss_lim, paced)

            if args.test_bidir:
                bidir_offer = (offer / 2.0) if paced else offer
                print(f"-- simultaneous ({bidir_offer:.0f} kbps each)")
                snaps, stopped = take_phase(
                    [
                        (dest_a, b"A", listen_b),
                        (dest_b, b"B", listen_a),
                    ],
                    bidir_offer,
                )
                result.bidir_ab, result.bidir_ba = snaps
                print_snap("A+B A", result.bidir_ab, listen_b.stats)
                print_snap("A+B B", result.bidir_ba, listen_a.stats)
                if stopped:
                    mark_int()
                    current = None
                    break
                result.bidir_ok = loss_ok(result.bidir_ab, 10.0, paced) and loss_ok(
                    result.bidir_ba, 10.0, paced
                )
            else:
                result.bidir_ok = True

            result.overall = (
                result.config_ok and result.integrity_ok and result.uni_ok and result.bidir_ok
            )
            all_results.append(result)
            current = None
    except KeyboardInterrupt:
        interrupted = True
        print("interrupted")
        if current is not None and (not all_results or all_results[-1] is not current):
            current.note = current.note or "INT"
            current.overall = False
            all_results.append(current)
    finally:
        listen_a.stop()
        listen_b.stop()

    print("\n=== result (interrupted) ===" if interrupted else "\n=== result ===")
    paced = args.kbps != 0
    print(
        fmt_summary(
            all_results,
            paced,
            integ=args.test_integ,
            ab=args.test_ab,
            ba=args.test_ba,
            bidir=args.test_bidir,
        )
    )

    passed = sum(1 for r in all_results if r.overall)
    if interrupted:
        return 130
    return 0 if passed == len(all_results) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
        raise SystemExit(130)
