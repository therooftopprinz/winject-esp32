#!/usr/bin/env python3
"""WT32-ETH01 WInject bandwidth test.

UDP into one radio's upstream_rx, 802.11 in the air, UDP out the other
radio's upstream_tx to this host. Measures integrity and payload goodput.
Sets CCA on both radios over the TCP console. Sets channel and modulation
only when --channel, --modulation, or --all is given; omitted leaves the
radios as they are.

--fec starts tools/fec.py encode/decode proxies on the host. The radios
still see opaque UDP; the test measures application payloads after decode.
"""

from __future__ import annotations

import argparse
import atexit
import select
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

CONSOLE_PORT = 2323
INJECT_PORT = 9000
HOST_PORT_A = 9001  # frames heard by radio A
HOST_PORT_B = 9002  # frames heard by radio B
MAX_PAYLOAD = 1476
FEC_SCRIPT = Path(__file__).resolve().parent / "fec.py"
FEC_ENC_A = 19100  # host -> encode -> radio A inject
FEC_ENC_B = 19110  # host -> encode -> radio B inject
FEC_APP_A = 19101  # decode of radio A upstream_tx (B -> A)
FEC_APP_B = 19102  # decode of radio B upstream_tx (A -> B)
FEC_BLOCK_HDR = 8
FEC_LEN_PREFIX = 2
FEC_INTRA_HDR = 2
MAX_BLOCK_ORIG = MAX_PAYLOAD - FEC_BLOCK_HDR - FEC_LEN_PREFIX  # 1466
DEFAULT_FEC_K = 8
DEFAULT_FEC_N = 12
DEFAULT_FEC_NSYM = 32
DEFAULT_FEC_TIMEOUT_MS = 20

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


@dataclass
class RecvStats:
    packets: int = 0
    nbytes: int = 0
    first: float | None = None
    last: float | None = None
    seqs: set[int] = field(default_factory=set)

    def clear(self) -> None:
        self.packets = 0
        self.nbytes = 0
        self.first = None
        self.last = None
        self.seqs.clear()

    def copy(self) -> RecvStats:
        return RecvStats(
            packets=self.packets,
            nbytes=self.nbytes,
            first=self.first,
            last=self.last,
            seqs=set(self.seqs),
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


def detect_host(peer: str) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((peer, CONSOLE_PORT))
        return sock.getsockname()[0]
    finally:
        sock.close()


def console(ip: str, cmds: list[str], timeout: float = 2.0, quiet: bool = False) -> list[str]:
    last_err: Exception | None = None
    for attempt in range(3):
        try:
            return _console_once(ip, cmds, timeout, quiet)
        except OSError as err:
            last_err = err
            time.sleep(1.0 + attempt)
    raise last_err  # type: ignore[misc]


def _console_once(ip: str, cmds: list[str], timeout: float, quiet: bool) -> list[str]:
    sock = socket.create_connection((ip, CONSOLE_PORT), timeout=timeout)
    sock.settimeout(0.8)
    replies: list[str] = []
    try:
        try:
            sock.recv(4096)
        except socket.timeout:
            pass
        for cmd in cmds:
            sock.sendall((cmd + "\n").encode())
            buf = b""
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    if buf:
                        break
                    continue
                if not chunk:
                    break
                buf += chunk
                if b"ok\n" in buf or b"error:" in buf or cmd == "status":
                    if cmd == "status":
                        sock.settimeout(0.25)
                        try:
                            while True:
                                extra = sock.recv(4096)
                                if not extra:
                                    break
                                buf += extra
                        except socket.timeout:
                            pass
                    break
            text = buf.decode("utf-8", "replace")
            replies.append(text)
            if not quiet:
                print(f"[{ip}] {cmd}")
                if text.strip():
                    print(text.rstrip())
            else:
                status = "ok" if "error:" not in text else text.strip().splitlines()[-1]
                print(f"[{ip}] {cmd}  {status}")
    finally:
        sock.close()
    return replies


def replies_ok(replies: list[str]) -> bool:
    return all("error:" not in text for text in replies)


def configure_upstream(ip: str, host: str, tx_port: int, quiet: bool) -> bool:
    replies = console(
        ip,
        [
            f"set_upstream_rx {INJECT_PORT}",
            f"set_upstream_tx {host} {tx_port}",
        ],
        quiet=quiet,
    )
    return replies_ok(replies)


def parse_status_field(text: str, key: str) -> str | None:
    for line in text.splitlines():
        parts = line.split()
        try:
            i = parts.index(key)
        except ValueError:
            continue
        if i + 1 < len(parts):
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


def log_status(ip: str, label: str) -> str:
    print(f"\n=== status {label} {ip} ===")
    try:
        return console(ip, ["status"], quiet=False)[0]
    except OSError as err:
        print(f"status failed: {err}")
        return ""


def configure_radio(
    ip: str, channel: int | None, modulation: str | None, cca: bool, quiet: bool
) -> bool:
    cmds: list[str] = []
    if channel is not None:
        cmds.append(f"set_channel {channel}")
    if modulation is not None:
        cmds.append(f"set_modulation {modulation}")
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


class Listener:
    def __init__(self, bind_ip: str, port: int) -> None:
        self.stats = RecvStats()
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

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data, _addr = self._sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            now = time.monotonic()
            st = self.stats
            if st.first is None:
                st.first = now
            st.last = now
            st.packets += 1
            st.nbytes += len(data)
            seq = parse_seq(data)
            if seq is not None:
                st.seqs.add(seq)


def send_exact(dest: tuple[str, int], tag: bytes, count: int, size: int, gap: float) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for seq in range(count):
            sock.sendto(make_payload(tag, seq, size), dest)
            if gap > 0:
                time.sleep(gap)
    finally:
        sock.close()
    return count


def auto_offer_kbps(modulation: str, size: int) -> float:
    """Payload offer slightly under estimated 802.11 goodput for this PHY."""
    phy = PHY_KBPS.get(modulation.upper(), 1000)
    preamble_us = 200.0 if phy <= 11000 else 40.0
    mac_us = 400.0 if phy <= 11000 else 150.0
    mpdu_bits = (24 + size) * 8
    air_us = preamble_us + (mpdu_bits / phy) * 1000.0 + mac_us
    return (size * 8.0) / (air_us / 1000.0) * 0.85


def intra_air_size(payload_len: int, nsym: int) -> int:
    if payload_len <= 0:
        return FEC_INTRA_HDR
    chunk = 255 - nsym
    if chunk < 1:
        raise SystemExit(f"--fec-nsym {nsym} is too large")
    n_chunks = (payload_len + chunk - 1) // chunk
    return FEC_INTRA_HDR + payload_len + n_chunks * nsym


def fec_max_orig(intra: bool, nsym: int) -> int:
    if not intra:
        return MAX_BLOCK_ORIG
    for size in range(MAX_PAYLOAD - FEC_INTRA_HDR, 0, -1):
        if intra_air_size(size, nsym) <= MAX_PAYLOAD:
            return size
    return 0


def fec_air_size(size: int, intra: bool, nsym: int) -> int:
    if intra:
        return intra_air_size(size, nsym)
    return min(MAX_PAYLOAD, size + FEC_BLOCK_HDR + FEC_LEN_PREFIX)


def auto_offer_kbps_fec(
    modulation: str, size: int, k: int, n: int, intra: bool, nsym: int
) -> float:
    """App-layer offer so air frames stay near the usual 85% PHY estimate."""
    air_size = fec_air_size(size, intra, nsym)
    air_offer = auto_offer_kbps(modulation, air_size)
    scale = size / air_size if air_size else 1.0
    if intra:
        return air_offer * scale
    return air_offer * (k / n) * scale


def fec_label(args: argparse.Namespace) -> str:
    if not args.fec:
        return "off"
    if args.fec_intra:
        return f"intra nsym={args.fec_nsym}"
    return f"block k={args.fec_k} n={args.fec_n} timeout={args.fec_timeout:g}ms"


class FecProxies:
    """Four host-side fec.py processes: encode A/B, decode A/B."""

    def __init__(
        self,
        radio_a: str,
        radio_b: str,
        k: int,
        n: int,
        nsym: int,
        timeout_ms: float,
        intra: bool,
        verbose: bool,
    ) -> None:
        self.radio_a = radio_a
        self.radio_b = radio_b
        self.k = k
        self.n = n
        self.nsym = nsym
        self.timeout_ms = timeout_ms
        self.intra = intra
        self.verbose = verbose
        self.procs: list[subprocess.Popen[str]] = []
        self._drains: list[threading.Thread] = []

    def start(self) -> None:
        if not FEC_SCRIPT.is_file():
            raise SystemExit(f"missing {FEC_SCRIPT}")
        common = [sys.executable, str(FEC_SCRIPT)]
        if self.intra:
            fec_args = ["--intra", "--nsym", str(self.nsym)]
        else:
            fec_args = ["--k", str(self.k), "--n", str(self.n), "--timeout", str(self.timeout_ms)]
        if self.verbose:
            fec_args.append("--verbose")
        specs = [
            (
                "enc-a",
                [
                    "--encode",
                    "--from",
                    str(FEC_ENC_A),
                    "--to",
                    f"{self.radio_a}:{INJECT_PORT}",
                    *fec_args,
                ],
            ),
            (
                "enc-b",
                [
                    "--encode",
                    "--from",
                    str(FEC_ENC_B),
                    "--to",
                    f"{self.radio_b}:{INJECT_PORT}",
                    *fec_args,
                ],
            ),
            (
                "dec-a",
                ["--decode", "--from", str(HOST_PORT_A), "--to", f"127.0.0.1:{FEC_APP_A}"],
            ),
            (
                "dec-b",
                ["--decode", "--from", str(HOST_PORT_B), "--to", f"127.0.0.1:{FEC_APP_B}"],
            ),
        ]
        try:
            for name, extra in specs:
                self._spawn(name, common + extra)
        except Exception:
            self.stop()
            raise

    def _spawn(self, name: str, argv: list[str]) -> None:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.procs.append(proc)
        deadline = time.monotonic() + 5.0
        started = False
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                err = proc.stderr.read() if proc.stderr else ""
                raise SystemExit(f"fec {name} exited {proc.returncode}: {err.strip()}")
            if proc.stderr is None:
                break
            ready = select_readable(proc.stderr, timeout=0.2)
            if not ready:
                continue
            line = proc.stderr.readline()
            if self.verbose and line:
                print(f"[fec {name}] {line}", end="")
            if "listening" in line:
                started = True
                break
        if not started:
            raise SystemExit(f"fec {name} did not print listening")
        t = threading.Thread(target=self._drain, args=(name, proc), daemon=True)
        t.start()
        self._drains.append(t)

    def _drain(self, name: str, proc: subprocess.Popen[str]) -> None:
        if proc.stderr is None:
            return
        for line in proc.stderr:
            if self.verbose:
                print(f"[fec {name}] {line}", end="")

    def stop(self) -> None:
        for proc in self.procs:
            if proc.poll() is None:
                proc.terminate()
        deadline = time.monotonic() + 2.0
        for proc in self.procs:
            remaining = deadline - time.monotonic()
            try:
                proc.wait(timeout=max(0.05, remaining))
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=1.0)
        self.procs.clear()


def select_readable(stream: object, timeout: float) -> bool:
    readable, _, _ = select.select([stream], [], [], timeout)
    return bool(readable)


def send_window(
    dest: tuple[str, int],
    tag: bytes,
    size: int,
    duration: float,
    kbps: float,
    start: float,
) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    seq = 0
    interval = (size * 8.0) / (kbps * 1000.0) if kbps > 0 else 0.0
    next_t = start
    try:
        while time.monotonic() - start < duration:
            now = time.monotonic()
            if interval > 0 and now < next_t:
                time.sleep(min(0.001, next_t - now))
                continue
            sock.sendto(make_payload(tag, seq, size), dest)
            sent += 1
            seq += 1
            if interval > 0:
                next_t += interval
                if next_t < time.monotonic() - interval:
                    next_t = time.monotonic()
    finally:
        sock.close()
    return sent


def goodput_kbps(stats: RecvStats, duration: float) -> float:
    if duration <= 0:
        return 0.0
    return (stats.nbytes * 8.0) / duration / 1000.0


def to_phase(sent: int, stats: RecvStats, duration: float) -> PhaseResult:
    return PhaseResult(sent=sent, recv=stats.packets, kbps=goodput_kbps(stats, duration))


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
    p = argparse.ArgumentParser(description="WT32-ETH01 WInject bandwidth test")
    p.add_argument("--a", default="192.168.253.11", help="radio A Ethernet IP")
    p.add_argument("--b", default="192.168.253.12", help="radio B Ethernet IP")
    p.add_argument("--host", default="", help="host IP that both radios send back to")
    p.add_argument(
        "--channel",
        type=int,
        default=None,
        help="set_channel on both radios (1-13); omit to keep existing",
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
    p.add_argument("--size", type=int, default=1400, help="UDP payload bytes (max 1476)")
    p.add_argument("--duration", type=float, default=5.0, help="seconds per bandwidth phase")
    p.add_argument("--drain", type=float, default=1.0, help="seconds to wait after sending")
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
        default=True,
        help="TX CCA / CSMA on both radios (default: enabled; --no-cca disables)",
    )
    p.add_argument("--skip-config", action="store_true", help="do not touch the TCP console")
    p.add_argument("--bidir", action="store_true", help="run simultaneous A+B phase")
    p.add_argument("--verbose", action="store_true", help="print full console replies")
    p.add_argument(
        "--fec",
        action="store_true",
        help="send/receive through tools/fec.py (app-layer loss after decode)",
    )
    p.add_argument(
        "--fec-intra",
        action="store_true",
        help="per-frame RS instead of packet-block FEC (implies --fec)",
    )
    p.add_argument("--fec-k", type=int, default=DEFAULT_FEC_K, help="FEC data packets per block")
    p.add_argument("--fec-n", type=int, default=DEFAULT_FEC_N, help="FEC total packets per block")
    p.add_argument("--fec-nsym", type=int, default=DEFAULT_FEC_NSYM, help="intra RS parity bytes")
    p.add_argument(
        "--fec-timeout",
        type=float,
        default=DEFAULT_FEC_TIMEOUT_MS,
        metavar="MS",
        help="encode partial-block flush timeout in ms",
    )
    return p.parse_args()


def fmt_summary(results: list[ModResult], paced: bool, bidir: bool) -> str:
    header = (
        "| modulation      | ch | offer kbps |  int A->B |  int B->A |"
        " A->B kbps | A->B loss | B->A kbps | B->A loss |"
    )
    if bidir:
        header += " A+B A kbps | A+B B kbps | A+B A loss | A+B B loss |"
    header += " result |"
    sep = (
        "|-----------------|---:|-----------:|----------:|----------:"
        "|----------:|----------:|----------:|----------:|"
    )
    if bidir:
        sep += "-----------:|-----------:|-----------:|-----------:|"
    sep += "--------|"
    lines = [header, sep]
    for r in results:
        int_ab = f"{r.integrity_ab}" if r.config_ok else "-"
        int_ba = f"{r.integrity_ba}" if r.config_ok else "-"
        def cell(phase: PhaseResult) -> tuple[str, str]:
            if not r.config_ok or phase.sent == 0:
                return "-", "-"
            loss = f"{phase.loss:.1f}" if paced else "n/a"
            return f"{phase.kbps:.1f}", loss

        ab_k, ab_l = cell(r.uni_ab)
        ba_k, ba_l = cell(r.uni_ba)
        verdict = "PASS" if r.overall else "FAIL"
        if r.note:
            verdict = r.note
        ch = "-" if r.channel is None else r.channel
        line = (
            f"| {r.modulation:<15} | {ch:>2} | {r.offer:>10.0f} |"
            f" {int_ab:>9} | {int_ba:>9} |"
            f" {ab_k:>9} | {ab_l:>9} | {ba_k:>9} | {ba_l:>9} |"
        )
        if bidir:
            a_k, a_l = cell(r.bidir_ab)
            b_k, b_l = cell(r.bidir_ba)
            line += f" {a_k:>10} | {b_k:>10} | {a_l:>10} | {b_l:>10} |"
        line += f" {verdict:<6} |"
        lines.append(line)
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    if args.fec_intra:
        args.fec = True
    if args.size < 16 or args.size > MAX_PAYLOAD:
        raise SystemExit(f"--size must be 16..{MAX_PAYLOAD}")
    if args.fec:
        if not 1 <= args.fec_k < args.fec_n <= 255:
            raise SystemExit(f"need 1 <= --fec-k < --fec-n <= 255, got k={args.fec_k} n={args.fec_n}")
        if not 1 <= args.fec_nsym <= 254:
            raise SystemExit(f"--fec-nsym must be 1..254, got {args.fec_nsym}")
        if args.fec_timeout < 0:
            raise SystemExit("--fec-timeout must be >= 0")
        max_orig = fec_max_orig(args.fec_intra, args.fec_nsym)
        if args.size > max_orig:
            raise SystemExit(
                f"--size {args.size} exceeds FEC max {max_orig} "
                f"({'intra' if args.fec_intra else 'block'} must fit in {MAX_PAYLOAD} on the air)"
            )
    if args.channel is not None and (args.channel < 1 or args.channel > 13):
        raise SystemExit("--channel must be 1-13")
    if args.all and args.modulation is not None:
        raise SystemExit("use --all or --modulation, not both")

    apply_modulation = args.all or args.modulation is not None
    if args.all:
        mods = list(MODULATIONS)
    elif args.modulation is not None:
        mods = parse_modulations(args.modulation)
    else:
        mods = []

    host = args.host or detect_host(args.a)
    quiet = not args.verbose
    cca_label = "enabled" if args.cca else "disabled"
    print(f"host {host}")

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
        mods = [current]

    channel_label = str(display_channel) if display_channel is not None else "existing"
    if args.channel is None:
        channel_label += " (unchanged)"
    mod_label_suffix = " (unchanged)" if not apply_modulation else ""
    print(f"\nA {args.a}  B {args.b}  channel {channel_label}  cca {cca_label}")
    print(f"modulations ({len(mods)}): {' '.join(mods)}{mod_label_suffix}")
    print(f"payload {args.size} B  duration {args.duration}s  drain {args.drain}s")
    print(f"fec {fec_label(args)}")

    if not args.skip_config:
        print("\n=== configure upstream ===")
        if not configure_upstream(args.a, host, HOST_PORT_A, quiet):
            raise SystemExit("failed to configure upstream on A")
        if not configure_upstream(args.b, host, HOST_PORT_B, quiet):
            raise SystemExit("failed to configure upstream on B")

    fec: FecProxies | None = None
    if args.fec:
        print("\n=== start fec proxies ===")
        fec = FecProxies(
            radio_a=args.a,
            radio_b=args.b,
            k=args.fec_k,
            n=args.fec_n,
            nsym=args.fec_nsym,
            timeout_ms=args.fec_timeout,
            intra=args.fec_intra,
            verbose=args.verbose,
        )
        fec.start()
        atexit.register(fec.stop)
        listen_a = Listener("127.0.0.1", FEC_APP_A)
        listen_b = Listener("127.0.0.1", FEC_APP_B)
        dest_a = ("127.0.0.1", FEC_ENC_A)
        dest_b = ("127.0.0.1", FEC_ENC_B)
        print(
            f"encode {dest_a[1]}/{dest_b[1]} -> radios:{INJECT_PORT}; "
            f"decode {HOST_PORT_A}/{HOST_PORT_B} -> app {FEC_APP_A}/{FEC_APP_B}"
        )
    else:
        listen_a = Listener(host, HOST_PORT_A)
        listen_b = Listener(host, HOST_PORT_B)
        dest_a = (args.a, INJECT_PORT)
        dest_b = (args.b, INJECT_PORT)

    listen_a.start()
    listen_b.start()
    time.sleep(0.2)

    def reset() -> None:
        listen_a.stats.clear()
        listen_b.stats.clear()

    def run_integrity() -> tuple[int, int]:
        reset()
        send_exact(dest_a, b"A", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ab = listen_b.stats.packets
        reset()
        send_exact(dest_b, b"B", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ba = listen_a.stats.packets
        return ab, ba

    def phase(
        senders: list[tuple[tuple[str, int], bytes, Listener]],
        kbps: float,
    ) -> list[PhaseResult]:
        reset()
        start = time.monotonic()
        results = [0] * len(senders)
        threads: list[threading.Thread] = []

        def run_one(idx: int, dest: tuple[str, int], tag: bytes) -> None:
            results[idx] = send_window(dest, tag, args.size, args.duration, kbps, start)

        for i, (dest, tag, _lis) in enumerate(senders):
            t = threading.Thread(target=run_one, args=(i, dest, tag))
            threads.append(t)
            t.start()
        for t in threads:
            t.join()
        time.sleep(args.drain)
        out: list[PhaseResult] = []
        for i, (_dest, _tag, lis) in enumerate(senders):
            out.append(to_phase(results[i], lis.stats, args.duration))
        return out

    all_results: list[ModResult] = []

    for idx, mod in enumerate(mods, 1):
        offer = args.kbps
        if offer < 0:
            if args.fec:
                offer = auto_offer_kbps_fec(
                    mod, args.size, args.fec_k, args.fec_n, args.fec_intra, args.fec_nsym
                )
            else:
                offer = auto_offer_kbps(mod, args.size)
        paced = offer > 0
        print(
            f"\n=== [{idx}/{len(mods)}] ch {channel_label}  {mod}{mod_label_suffix}  "
            f"cca {cca_label}  fec {fec_label(args)}  offer {offer:.0f} kbps ==="
        )
        result = ModResult(modulation=mod, channel=display_channel, offer=offer)

        if not args.skip_config:
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
                continue
            if not ok_a or not ok_b:
                result.config_ok = False
                result.note = "CONFIG"
                result.overall = False
                print("radio config failed")
                all_results.append(result)
                continue
            if args.channel is not None or apply_modulation:
                time.sleep(1.2)

        print("-- integrity")
        a_to_b, b_to_a = run_integrity()
        print(f"A -> B  {a_to_b}/{args.integrity}")
        print(f"B -> A  {b_to_a}/{args.integrity}")
        if a_to_b != args.integrity or b_to_a != args.integrity:
            print("integrity retry")
            time.sleep(0.5)
            a_to_b, b_to_a = run_integrity()
            print(f"A -> B  {a_to_b}/{args.integrity}")
            print(f"B -> A  {b_to_a}/{args.integrity}")
        result.integrity_ab = a_to_b
        result.integrity_ba = b_to_a
        result.integrity_ok = a_to_b == args.integrity and b_to_a == args.integrity

        print("-- unidirectional")
        result.uni_ab = phase([(dest_a, b"A", listen_b)], offer)[0]
        result.uni_ba = phase([(dest_b, b"B", listen_a)], offer)[0]
        print(
            f"A->B sent {result.uni_ab.sent} recv {result.uni_ab.recv}  "
            f"{result.uni_ab.kbps:.1f} kbps  loss {result.uni_ab.loss:.1f}%"
        )
        print(
            f"B->A sent {result.uni_ba.sent} recv {result.uni_ba.recv}  "
            f"{result.uni_ba.kbps:.1f} kbps  loss {result.uni_ba.loss:.1f}%"
        )
        result.uni_ok = loss_ok(result.uni_ab, 5.0, paced) and loss_ok(result.uni_ba, 5.0, paced)

        if args.bidir:
            bidir_offer = (offer / 2.0) if paced else offer
            print(f"-- simultaneous ({bidir_offer:.0f} kbps each)")
            snaps = phase(
                [
                    (dest_a, b"A", listen_b),
                    (dest_b, b"B", listen_a),
                ],
                bidir_offer,
            )
            result.bidir_ab, result.bidir_ba = snaps
            print(
                f"A+B A sent {result.bidir_ab.sent} recv {result.bidir_ab.recv}  "
                f"{result.bidir_ab.kbps:.1f} kbps  loss {result.bidir_ab.loss:.1f}%"
            )
            print(
                f"A+B B sent {result.bidir_ba.sent} recv {result.bidir_ba.recv}  "
                f"{result.bidir_ba.kbps:.1f} kbps  loss {result.bidir_ba.loss:.1f}%"
            )
            result.bidir_ok = loss_ok(result.bidir_ab, 10.0, paced) and loss_ok(
                result.bidir_ba, 10.0, paced
            )
        else:
            result.bidir_ok = True

        result.overall = result.config_ok and result.integrity_ok and result.uni_ok and result.bidir_ok
        print("PASS" if result.overall else "FAIL")
        all_results.append(result)

    listen_a.stop()
    listen_b.stop()
    if fec is not None:
        fec.stop()

    print("\n=== result ===")
    paced = args.kbps != 0
    print(fmt_summary(all_results, paced, args.bidir))

    passed = sum(1 for r in all_results if r.overall)
    print(f"\n{passed}/{len(all_results)} modulations PASS  channel {channel_label}")
    return 0 if passed == len(all_results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
