#!/usr/bin/env python3
"""WT32-ETH01 WInject bandwidth test.

UDP into one radio's upstream_rx, 802.11 in the air, UDP out the other
radio's upstream_tx to this host. Measures integrity and payload goodput.
Sets channel, modulation, and CCA on both radios over the TCP console.
"""

from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from dataclasses import dataclass, field

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

CONSOLE_PORT = 2323
INJECT_PORT = 9000
HOST_PORT_A = 9001  # frames heard by radio A
HOST_PORT_B = 9002  # frames heard by radio B
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
    channel: int
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


def configure_radio(
    ip: str, channel: int, modulation: str, cca: bool, quiet: bool
) -> bool:
    replies = console(
        ip,
        [
            f"set_channel {channel}",
            f"set_modulation {modulation}",
            f"set_cca_enabled {1 if cca else 0}",
        ],
        quiet=quiet,
    )
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
        return list(MODULATIONS)
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
    p.add_argument("--channel", type=int, default=1, help="set_channel on both radios (1-13)")
    p.add_argument(
        "--modulation",
        default="all",
        help="one name, comma-separated list, or 'all' (default: all)",
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
    p.add_argument("--skip-bidir", action="store_true", help="skip simultaneous A+B phase")
    p.add_argument("--verbose", action="store_true", help="print full console replies")
    return p.parse_args()


def fmt_summary(results: list[ModResult], paced: bool, skip_bidir: bool) -> str:
    header = (
        "| modulation      | ch | offer kbps |  int A->B |  int B->A |"
        " A->B kbps | A->B loss | B->A kbps | B->A loss |"
    )
    if not skip_bidir:
        header += " A+B A kbps | A+B B kbps | A+B A loss | A+B B loss |"
    header += " result |"
    sep = (
        "|-----------------|---:|-----------:|----------:|----------:"
        "|----------:|----------:|----------:|----------:|"
    )
    if not skip_bidir:
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
        line = (
            f"| {r.modulation:<15} | {r.channel:>2} | {r.offer:>10.0f} |"
            f" {int_ab:>9} | {int_ba:>9} |"
            f" {ab_k:>9} | {ab_l:>9} | {ba_k:>9} | {ba_l:>9} |"
        )
        if not skip_bidir:
            a_k, a_l = cell(r.bidir_ab)
            b_k, b_l = cell(r.bidir_ba)
            line += f" {a_k:>10} | {b_k:>10} | {a_l:>10} | {b_l:>10} |"
        line += f" {verdict:<6} |"
        lines.append(line)
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    if args.size < 16 or args.size > MAX_PAYLOAD:
        raise SystemExit(f"--size must be 16..{MAX_PAYLOAD}")
    if args.channel < 1 or args.channel > 13:
        raise SystemExit("--channel must be 1-13")

    mods = parse_modulations(args.modulation)
    host = args.host or detect_host(args.a)
    quiet = not args.verbose
    print(f"host {host}")
    cca_label = "enabled" if args.cca else "disabled"
    print(f"A {args.a}  B {args.b}  channel {args.channel}  cca {cca_label}")
    print(f"modulations ({len(mods)}): {' '.join(mods)}")
    print(f"payload {args.size} B  duration {args.duration}s  drain {args.drain}s")

    if not args.skip_config:
        print("\n=== configure upstream ===")
        if not configure_upstream(args.a, host, HOST_PORT_A, quiet):
            raise SystemExit("failed to configure upstream on A")
        if not configure_upstream(args.b, host, HOST_PORT_B, quiet):
            raise SystemExit("failed to configure upstream on B")

    listen_a = Listener(host, HOST_PORT_A)
    listen_b = Listener(host, HOST_PORT_B)
    listen_a.start()
    listen_b.start()
    time.sleep(0.2)

    dest_a = (args.a, INJECT_PORT)
    dest_b = (args.b, INJECT_PORT)

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
            offer = auto_offer_kbps(mod, args.size)
        paced = offer > 0
        print(
            f"\n=== [{idx}/{len(mods)}] ch {args.channel}  {mod}  "
            f"cca {cca_label}  offer {offer:.0f} kbps ==="
        )
        result = ModResult(modulation=mod, channel=args.channel, offer=offer)

        if not args.skip_config:
            try:
                ok_a = configure_radio(args.a, args.channel, mod, args.cca, quiet)
                ok_b = configure_radio(args.b, args.channel, mod, args.cca, quiet)
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
                print("set_channel/set_modulation/set_cca_enabled failed")
                all_results.append(result)
                continue
            time.sleep(0.8)

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

        if not args.skip_bidir:
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

    print("\n=== result ===")
    paced = args.kbps != 0
    print(fmt_summary(all_results, paced, args.skip_bidir))

    passed = sum(1 for r in all_results if r.overall)
    print(f"\n{passed}/{len(all_results)} modulations PASS  channel {args.channel}")
    return 0 if passed == len(all_results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
