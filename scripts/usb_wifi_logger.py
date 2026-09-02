#!/usr/bin/env python3
"""Sniff a USB WiFi monitor iface for WInject air frames and decode bw_test.

Captures radiotap + 802.11 on a monitor interface (default: mon0), keeps frames
whose Addr3 is the tunnel BSSID used by tools/bw_test.py, unwraps the IBSS
body, and tries to parse:

  A 00000000 …   /   B 00000000 …     plain bw_test UDP payload
  0xF1 block FEC                       tools/fec.py packet-block
  0xF2 intra FEC                       tools/fec.py per-frame RS

    sudo python3 scripts/usb_wifi_logger.py
    sudo python3 scripts/usb_wifi_logger.py --iface mon0 --channel 1
    sudo python3 scripts/usb_wifi_logger.py --quiet --pcap /tmp/winject.pcap
    python3 scripts/usb_wifi_logger.py --self-test
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

BSSID_TUNNEL = bytes.fromhex("baddcafebabe")
BSSID_STANDALONE = bytes.fromhex("deadcafebabe")
BROADCAST = bytes.fromhex("ffffffffffff")
ETH_P_ALL = 3
LINKTYPE_IEEE802_11_RADIOTAP = 127
WIFI_HDR_LEN = 24
WIFI_QOS_HDR_LEN = 26
MAGIC_BLOCK = 0xF1
MAGIC_INTRA = 0xF2
FLAG_PARITY = 0x01
RT_FCS = 0x10
RT_BAD_FCS = 0x40
RT_SHORT_PREAMBLE = 0x02
RT_EXT = 1 << 31
RT_VENDOR_NS = 1 << 30

RATE_NAME = {
    2: "DSS_1M_L",
    4: "DSS_2M",
    11: "CCK_5M",
    22: "CCK_11M",
    12: "OFDM_6M",
    18: "OFDM_9M",
    24: "OFDM_12M",
    36: "OFDM_18M",
    48: "OFDM_24M",
    72: "OFDM_36M",
    96: "OFDM_48M",
    108: "OFDM_54M",
}

# (align, size) for default-namespace radiotap bits 0..27.
RT_FIELDS: dict[int, tuple[int, int]] = {
    0: (8, 8),  # TSFT
    1: (1, 1),  # Flags
    2: (1, 1),  # Rate
    3: (2, 4),  # Channel
    4: (2, 2),  # FHSS
    5: (1, 1),  # dBm antsignal
    6: (1, 1),  # dBm antnoise
    7: (2, 2),  # lock quality
    8: (2, 2),  # TX attenuation
    9: (2, 2),  # dB TX attenuation
    10: (1, 1),  # dBm TX power
    11: (1, 1),  # antenna
    12: (1, 1),  # dB antsignal
    13: (1, 1),  # dB antnoise
    14: (2, 2),  # RX flags
    15: (2, 2),  # TX flags
    16: (1, 1),  # RTS retries
    17: (1, 1),  # data retries
    18: (4, 8),  # XChannel
    19: (1, 3),  # MCS
    20: (4, 8),  # AMPDU
    21: (2, 12),  # VHT
    22: (8, 12),  # timestamp
    23: (2, 12),  # HE
    24: (2, 12),  # HE-MU
    25: (2, 6),  # HE-MU other user
    26: (1, 1),  # 0-length PSDU
    27: (2, 4),  # L-SIG
}


def _add_user_site_packages() -> None:
    """sudo python3 does not see pip --user packages; add SUDO_USER's site-packages."""
    homes: list[str] = []
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user and sudo_user != "root":
        homes.append(str(Path("/home") / sudo_user))
    home = os.path.expanduser("~")
    if home not in homes:
        homes.append(home)
    ver = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for h in homes:
        extra = str(Path(h) / ".local" / "lib" / ver / "site-packages")
        if os.path.isdir(extra) and extra not in sys.path:
            sys.path.insert(0, extra)


def _load_fec():
    _add_user_site_packages()
    try:
        import reedsolo  # noqa: F401
    except ImportError:
        return None
    try:
        import fec as fec_mod  # type: ignore

        return fec_mod
    except (ImportError, SystemExit):
        return None


FEC = _load_fec()


def mac_str(raw: bytes) -> str:
    return ":".join(f"{b:02x}" for b in raw)


def parse_mac(text: str) -> bytes:
    hexed = text.replace(":", "").replace("-", "").replace(".", "").lower()
    if len(hexed) != 12:
        raise argparse.ArgumentTypeError(f"MAC must be 6 bytes, got {text!r}")
    try:
        return bytes.fromhex(hexed)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid MAC {text!r}") from exc


def align_up(off: int, width: int) -> int:
    if width <= 1:
        return off
    return (off + width - 1) & ~(width - 1)


@dataclass
class Radiotap:
    length: int
    flags: int = 0
    rate: int | None = None
    freq: int | None = None
    dbm: int | None = None
    noise: int | None = None
    mcs: int | None = None
    sgi: bool = False
    ht40: bool = False
    vht_mcs: int | None = None
    vht_nss: int | None = None
    fcs: bool = False
    bad_fcs: bool = False

    @property
    def phy(self) -> str:
        if self.mcs is not None:
            gi = "SGI" if self.sgi else "LGI"
            return f"OFDM_MCS{self.mcs}_{gi}"
        if self.vht_mcs is not None:
            nss = self.vht_nss or 1
            return f"VHT_MCS{self.vht_mcs}_NSS{nss}"
        if self.rate is None:
            return "?"
        name = RATE_NAME.get(self.rate)
        if name in ("DSS_2M", "CCK_5M", "CCK_11M"):
            name = name + ("_S" if self.flags & RT_SHORT_PREAMBLE else "_L")
        return name or f"{self.rate * 500 / 1000:.1f}M"


def parse_radiotap(pkt: bytes) -> Radiotap | None:
    if len(pkt) < 8:
        return None
    version, _pad, it_len, present0 = struct.unpack_from("<BBHI", pkt, 0)
    if version != 0 or it_len < 8 or it_len > len(pkt):
        return None
    presents = [present0]
    off = 8
    word = present0
    while word & RT_EXT:
        if off + 4 > it_len:
            return Radiotap(length=it_len)
        word = struct.unpack_from("<I", pkt, off)[0]
        presents.append(word)
        off += 4

    info = Radiotap(length=it_len)
    for word in presents:
        if word & RT_VENDOR_NS:
            off = align_up(off, 2)
            if off + 6 > it_len:
                break
            _oui0, _oui1, _oui2, _sub, skip = struct.unpack_from("<BBBBH", pkt, off)
            off += 6 + skip
            continue
        for bit in range(0, 29):
            if not (word & (1 << bit)):
                continue
            spec = RT_FIELDS.get(bit)
            if spec is None:
                return info
            al, size = spec
            off = align_up(off, al)
            if off + size > it_len:
                return info
            field = pkt[off : off + size]
            off += size
            if bit == 1:
                info.flags = field[0]
                info.fcs = bool(info.flags & RT_FCS)
                info.bad_fcs = bool(info.flags & RT_BAD_FCS)
            elif bit == 2:
                info.rate = field[0]
            elif bit == 3:
                info.freq = struct.unpack_from("<H", field, 0)[0]
            elif bit == 5:
                dbm = struct.unpack("<b", field[:1])[0]
                if info.dbm is None:
                    info.dbm = dbm
            elif bit == 6:
                info.noise = struct.unpack("<b", field[:1])[0]
            elif bit == 19:
                known, flags, mcs = field[0], field[1], field[2]
                if known & 0x02:
                    info.mcs = mcs
                if known & 0x04:
                    info.sgi = bool(flags & 0x04)
                if known & 0x01:
                    info.ht40 = bool(flags & 0x03)
            elif bit == 21:
                known = struct.unpack_from("<H", field, 0)[0]
                mcs_nss = field[4]
                if known & 0x04 and mcs_nss:
                    info.vht_nss = mcs_nss & 0x0F
                    info.vht_mcs = mcs_nss >> 4
                gi_known = known & 0x04
                if gi_known:
                    info.sgi = bool(field[2] & 0x04)
    return info


@dataclass
class Dot11:
    fc: int
    da: bytes
    sa: bytes
    bssid: bytes
    seq: int
    retry: bool
    qos: bool
    hdr_len: int
    body: bytes
    ftype: int
    stype: int

    @property
    def is_data(self) -> bool:
        return self.ftype == 2


def parse_dot11(mpdu: bytes, fcs: bool) -> Dot11 | None:
    raw = mpdu[:-4] if fcs and len(mpdu) >= 4 else mpdu
    if len(raw) < WIFI_HDR_LEN:
        return None
    fc, _dur = struct.unpack_from("<HH", raw, 0)
    ftype = (fc >> 2) & 0x3
    stype = (fc >> 4) & 0xF
    qos = ftype == 2 and stype == 8
    hdr_len = WIFI_QOS_HDR_LEN if qos else WIFI_HDR_LEN
    if qos and len(raw) < hdr_len:
        return None
    to_ds = bool(fc & 0x0100)
    from_ds = bool(fc & 0x0200)
    addr1 = raw[4:10]
    addr2 = raw[10:16]
    addr3 = raw[16:22]
    if not to_ds and not from_ds:
        da, sa, bssid = addr1, addr2, addr3
    elif to_ds and not from_ds:
        da, sa, bssid = addr3, addr2, addr1
    elif not to_ds and from_ds:
        da, sa, bssid = addr1, addr3, addr2
    else:
        da, sa, bssid = addr1, addr2, addr3
    seqctl = struct.unpack_from("<H", raw, 22)[0]
    return Dot11(
        fc=fc,
        da=da,
        sa=sa,
        bssid=addr3,
        seq=(seqctl >> 4) & 0x0FFF,
        retry=bool(fc & 0x0800),
        qos=qos,
        hdr_len=hdr_len,
        body=raw[hdr_len:],
        ftype=ftype,
        stype=stype,
    )


@dataclass
class BwTest:
    tag: str
    seq: int
    size: int


def parse_bw_test(payload: bytes) -> BwTest | None:
    if len(payload) < 11:
        return None
    if payload[1:2] != b" " or payload[10:11] != b" ":
        return None
    tag = payload[0:1]
    if tag not in (b"A", b"B"):
        return None
    try:
        seq = int(payload[2:10])
    except ValueError:
        return None
    return BwTest(tag=tag.decode("ascii"), seq=seq, size=len(payload))


def unpack_block_header(data: bytes) -> tuple[int, int, int, int, int] | None:
    if len(data) < 8:
        return None
    magic, version, block_id, index, k, n, flags = struct.unpack("!BBHBBBB", data[:8])
    if magic != MAGIC_BLOCK or version != 1:
        return None
    if not 1 <= k < n <= 255 or index >= n:
        return None
    return block_id, index, k, n, flags


@dataclass
class SeqTrack:
    last: int | None = None
    count: int = 0
    missing: int = 0
    dup: int = 0
    first_seq: int | None = None
    max_seq: int | None = None
    resets: int = 0
    sizes: set[int] = field(default_factory=set)

    def add(self, seq: int, size: int) -> str | None:
        self.count += 1
        self.sizes.add(size)
        if self.first_seq is None:
            self.first_seq = seq
        if self.max_seq is None or seq > self.max_seq:
            self.max_seq = seq
        note = None
        if self.last is None:
            self.last = seq
            return None
        if seq == self.last:
            self.dup += 1
            return "dup"
        if seq == 0 or seq + 10 < self.last:
            self.resets += 1
            self.last = seq
            return "reset"
        if seq == self.last + 1:
            self.last = seq
            return None
        if seq > self.last + 1:
            gap = seq - self.last - 1
            self.missing += gap
            note = f"gap {gap}"
            self.last = seq
            return note
        self.last = seq
        return None


@dataclass
class SaStats:
    pkts: int = 0
    bytes: int = 0
    fcs_fail: int = 0
    rssi_min: int | None = None
    rssi_max: int | None = None
    rssi_sum: int = 0
    rssi_n: int = 0
    tags: dict[str, SeqTrack] = field(default_factory=lambda: defaultdict(SeqTrack))

    def rssi(self, dbm: int | None) -> None:
        if dbm is None:
            return
        self.rssi_n += 1
        self.rssi_sum += dbm
        self.rssi_min = dbm if self.rssi_min is None else min(self.rssi_min, dbm)
        self.rssi_max = dbm if self.rssi_max is None else max(self.rssi_max, dbm)


class PcapWriter:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._fp = path.open("wb")
        self._fp.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_IEEE802_11_RADIOTAP))
        self.packets = 0

    def write(self, pkt: bytes, ts: float) -> None:
        sec = int(ts)
        usec = int((ts - sec) * 1_000_000)
        self._fp.write(struct.pack("<IIII", sec, usec, len(pkt), len(pkt)))
        self._fp.write(pkt)
        self.packets += 1

    def close(self) -> None:
        self._fp.close()


class Logger:
    def __init__(self, quiet: bool, hex_bytes: int) -> None:
        self.quiet = quiet
        self.hex_bytes = hex_bytes
        self.t0 = time.time()
        self.seen = 0
        self.match = 0
        self.nondata = 0
        self.other_bssid = 0
        self.bad_rt = 0
        self.fcs_fail = 0
        self.bw_test = 0
        self.fec_intra = 0
        self.fec_block = 0
        self.fec_decoded = 0
        self.fec_fail = 0
        self.unknown = 0
        self.body_bytes = 0
        self.by_sa: dict[bytes, SaStats] = defaultdict(SaStats)
        self.sa_name: dict[bytes, str] = {}
        self.decode_state = FEC.DecodeState() if FEC is not None else None
        self.last_summary = self.t0

    def elapsed(self) -> float:
        return time.time() - self.t0

    def handle_decoded(self, sa: bytes, payloads: list[bytes], recovered: bool) -> None:
        st = self.by_sa[sa]
        for payload in payloads:
            bw = parse_bw_test(payload)
            if bw is None:
                self.unknown += 1
                if not self.quiet:
                    print(f"         decoded {len(payload)}B {preview(payload, self.hex_bytes)}")
                continue
            self.fec_decoded += 1
            self.bw_test += 1
            self.sa_name.setdefault(sa, bw.tag)
            note = st.tags[bw.tag].add(bw.seq, bw.size)
            extra = []
            if recovered:
                extra.append("recovered")
            if note:
                extra.append(note)
            if not self.quiet:
                tail = f"  ({', '.join(extra)})" if extra else ""
                print(f"         decoded {bw.tag} seq={bw.seq} {bw.size}B{tail}")

    def on_frame(self, rt: Radiotap, dot: Dot11) -> None:
        self.match += 1
        self.body_bytes += len(dot.body)
        st = self.by_sa[dot.sa]
        st.pkts += 1
        st.bytes += len(dot.body)
        st.rssi(rt.dbm)
        if rt.bad_fcs:
            self.fcs_fail += 1
            st.fcs_fail += 1

        kind, decoded, recovered = classify_body(dot.body, self.decode_state)
        if kind.startswith("fec_intra"):
            self.fec_intra += 1
        elif kind.startswith("fec_block"):
            self.fec_block += 1
        elif kind.startswith("bw_test"):
            self.bw_test += 1
            bw = parse_bw_test(dot.body)
            if bw is not None:
                self.sa_name.setdefault(dot.sa, bw.tag)
                note = st.tags[bw.tag].add(bw.seq, bw.size)
                if note:
                    kind += f" {note}"
        elif kind.startswith("fec_fail"):
            self.fec_fail += 1
        else:
            self.unknown += 1

        if not self.quiet:
            print(format_line(self.elapsed(), rt, dot, kind, self.sa_name, self.hex_bytes))
        if decoded:
            self.handle_decoded(dot.sa, decoded, recovered)

    def summary(self, final: bool = False) -> None:
        dt = self.elapsed()
        kbps = (self.body_bytes * 8.0) / dt / 1000.0 if dt > 0 else 0.0
        head = "=== done ===" if final else "=== stats ==="
        print(
            f"{head}  {dt:.1f}s  iface_pkts={self.seen}  bssid={self.match}  "
            f"fcs_fail={self.fcs_fail}  other_bssid={self.other_bssid}  "
            f"air={kbps:.1f} kbps"
        )
        print(
            f"  bw_test={self.bw_test}  fec_block={self.fec_block}  "
            f"fec_intra={self.fec_intra}  fec_decoded={self.fec_decoded}  "
            f"fec_fail={self.fec_fail}  unknown={self.unknown}"
        )
        for sa, st in sorted(self.by_sa.items(), key=lambda kv: mac_str(kv[0])):
            name = self.sa_name.get(sa, "?")
            rssi = ""
            if st.rssi_n:
                avg = st.rssi_sum / st.rssi_n
                rssi = f"  rssi {st.rssi_min}..{st.rssi_max} avg {avg:.0f}"
            print(
                f"  {name}  {mac_str(sa)}  pkts={st.pkts}  {st.bytes}B  "
                f"fcs_fail={st.fcs_fail}{rssi}"
            )
            for tag, tr in sorted(st.tags.items()):
                sizes = ",".join(str(s) for s in sorted(tr.sizes))
                print(
                    f"      {tag}  n={tr.count}  seq {tr.first_seq}..{tr.max_seq}  "
                    f"miss={tr.missing}  dup={tr.dup}  reset={tr.resets}  size={sizes}"
                )


def preview(payload: bytes, n: int) -> str:
    if n <= 0:
        return ""
    chunk = payload[:n]
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
    return f"ascii={text!r} hex={chunk.hex()}"


def classify_body(
    body: bytes, decode_state: object | None
) -> tuple[str, list[bytes], bool]:
    if not body:
        return "empty", [], False
    bw = parse_bw_test(body)
    if bw is not None:
        return f"bw_test {bw.tag} seq={bw.seq}", [], False

    magic = body[0]
    if magic == MAGIC_INTRA:
        nsym = body[1] if len(body) > 1 else 0
        if decode_state is None or FEC is None:
            return f"fec_intra nsym={nsym}", [], False
        out = FEC.decode_feed(decode_state, body)
        if not out:
            return f"fec_fail intra nsym={nsym}", [], False
        return f"fec_intra nsym={nsym}", out, False

    if magic == MAGIC_BLOCK:
        hdr = unpack_block_header(body)
        if hdr is None:
            return "fec_fail block hdr", [], False
        block_id, index, k, n, flags = hdr
        role = "PARITY" if flags & FLAG_PARITY else "data"
        label = f"fec_block id={block_id} i={index}/{n} k={k} {role}"
        if decode_state is None or FEC is None:
            return label, [], False
        before_rec = getattr(decode_state, "stats").recovered
        out = FEC.decode_feed(decode_state, body)
        recovered = getattr(decode_state, "stats").recovered > before_rec
        if out:
            return label, out, recovered
        return label, [], False

    peek = "".join(chr(b) if 32 <= b < 127 else "." for b in body[:12])
    return f"data {len(body)}B {peek!r}", [], False


def format_line(
    elapsed: float,
    rt: Radiotap,
    dot: Dot11,
    kind: str,
    names: dict[bytes, str],
    hex_bytes: int,
) -> str:
    rssi = f"{rt.dbm:>4}dBm" if rt.dbm is not None else "    ?dBm"
    freq = f"{rt.freq}MHz" if rt.freq is not None else "?MHz"
    name = names.get(dot.sa)
    sa = f"{name} {mac_str(dot.sa)}" if name else mac_str(dot.sa)
    flags = []
    if rt.bad_fcs:
        flags.append("BAD_FCS")
    if dot.retry:
        flags.append("retry")
    flag_s = f"  {' '.join(flags)}" if flags else ""
    hex_s = ""
    if hex_bytes > 0 and not kind.startswith("bw_test"):
        extra = preview(dot.body, hex_bytes)
        if extra:
            hex_s = f"  {extra}"
    return (
        f"{elapsed:10.6f}  {rssi}  {freq:<8}  {rt.phy:<16}  "
        f"{len(dot.body):4}B  SA={sa}  802.11seq={dot.seq:<4}  {kind}{flag_s}{hex_s}"
    )


def list_monitor_ifaces() -> list[str]:
    try:
        out = subprocess.check_output(["iw", "dev"], text=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    ifaces: list[str] = []
    name = None
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.startswith("Interface "):
            name = stripped.split()[1]
        elif stripped.startswith("type ") and name:
            if stripped.split()[1] == "monitor":
                ifaces.append(name)
            name = None
    return ifaces


def resolve_iface(requested: str | None) -> str:
    monitors = list_monitor_ifaces()
    if requested:
        if requested not in monitors:
            print(
                f"warning: {requested} is not a monitor iface "
                f"(monitor: {', '.join(monitors) or 'none'})",
                file=sys.stderr,
            )
        return requested
    if "mon0" in monitors:
        return "mon0"
    if monitors:
        return monitors[0]
    raise SystemExit("no monitor-mode WiFi interface found (expected mon0)")


def wiphy_for(iface: str) -> str | None:
    try:
        out = subprocess.check_output(["iw", "dev", iface, "info"], text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines():
        if "wiphy" in line:
            return f"phy{line.split()[-1]}"
    return None


def set_channel(iface: str, channel: int) -> None:
    phy = wiphy_for(iface)
    if phy is None:
        raise SystemExit(f"could not find wiphy for {iface}")
    try:
        subprocess.check_call(["iw", "phy", phy, "set", "channel", str(channel), "HT20"])
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"failed to set {phy} channel {channel}: {exc}") from exc
    except FileNotFoundError as exc:
        raise SystemExit("iw not found") from exc


def open_monitor(iface: str) -> socket.socket:
    try:
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    except PermissionError as exc:
        raise SystemExit("need root for AF_PACKET (sudo python3 scripts/usb_wifi_logger.py)") from exc
    sock.bind((iface, ETH_P_ALL))
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    sock.settimeout(0.25)
    return sock


def make_radiotap(flags: int = 0, rate: int = 12, freq: int = 2412, dbm: int = -50) -> bytes:
    present = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5)
    fields = struct.pack("<BBHHb", flags, rate, freq, 0x00A0, dbm)
    it_len = 8 + len(fields)
    return struct.pack("<BBHI", 0, 0, it_len, present) + fields


def make_data_mpdu(sa: bytes, bssid: bytes, body: bytes, seq: int = 1, fcs: bytes | None = None) -> bytes:
    fc = 0x0008
    hdr = struct.pack("<HH", fc, 0) + BROADCAST + sa + bssid + struct.pack("<H", (seq & 0x0FFF) << 4)
    mpdu = hdr + body
    if fcs is not None:
        mpdu += fcs
    return mpdu


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)

    rt_bytes = make_radiotap(flags=RT_FCS, rate=24, freq=2412, dbm=-42)
    rt = parse_radiotap(rt_bytes + b"\x00" * 8)
    expect(rt is not None and rt.freq == 2412 and rt.dbm == -42, "radiotap channel/rssi")
    expect(rt is not None and rt.phy == "OFDM_12M" and rt.fcs, f"radiotap phy={rt.phy if rt else None}")

    # rtw88 / Archer T3U: three present words, per-antenna signal, FCS at end.
    rtw = bytes.fromhex(
        "000026002f4000a0200800a02008000080c183290000000010026c09a000cd000000cd000001"
        "80000000ffffffffffff927abe6c4294927abe6c429470e9fe2fd52200000000"
    )
    rt_rtw = parse_radiotap(rtw)
    expect(
        rt_rtw is not None and rt_rtw.length == 38 and rt_rtw.freq == 2412 and rt_rtw.fcs,
        f"rtw88 radiotap meta {rt_rtw}",
    )
    expect(rt_rtw is not None and rt_rtw.dbm == -51 and rt_rtw.phy == "DSS_1M_L", f"rtw88 rssi/phy {rt_rtw}")
    dot_rtw = parse_dot11(rtw[rt_rtw.length :], rt_rtw.fcs) if rt_rtw else None
    expect(
        dot_rtw is not None and dot_rtw.ftype == 0 and mac_str(dot_rtw.bssid) == "92:7a:be:6c:42:94",
        f"rtw88 beacon {dot_rtw}",
    )

    sa = bytes.fromhex("20500d303920")
    body = b"A 00000007 " + bytes(53)
    mpdu = make_data_mpdu(sa, BSSID_TUNNEL, body, seq=9, fcs=b"\x11\x22\x33\x44")
    pkt = rt_bytes + mpdu
    rt2 = parse_radiotap(pkt)
    assert rt2 is not None
    dot = parse_dot11(pkt[rt2.length :], rt2.fcs)
    expect(dot is not None and dot.sa == sa and dot.bssid == BSSID_TUNNEL, "dot11 addrs")
    expect(dot is not None and dot.is_data and parse_bw_test(dot.body) == BwTest("A", 7, 64), "bw_test unwrap")

    bw = parse_bw_test(b"B 00001234 " + b"\x00" * 20)
    expect(bw == BwTest("B", 1234, 31), f"parse_bw_test {bw}")
    expect(parse_bw_test(b"nope") is None, "reject non bw_test")

    if FEC is None:
        failures.append("fec.py / reedsolo not available")
    else:
        sample = b"A 00000003 " + bytes(50)
        intra = FEC.encode_intra(sample, 32)
        expect(intra is not None and intra[0] == MAGIC_INTRA, "intra encode")
        state = FEC.DecodeState()
        kind, out, _rec = classify_body(intra, state)
        expect(kind.startswith("fec_intra") and out == [sample], f"intra classify {kind} {out!r}")
        parsed = parse_bw_test(out[0]) if out else None
        expect(parsed == BwTest("A", 3, 61), f"intra bw_test {parsed}")

        orig = [b"A 00000000 " + bytes(20), b"A 00000001 " + bytes(20)]
        pkts = FEC.encode_block(orig, 8, 12, 7)
        expect(len(pkts) == 12 and pkts[0][0] == MAGIC_BLOCK, "block encode")
        state = FEC.DecodeState()
        decoded: list[bytes] = []
        for i, pkt_body in enumerate(pkts):
            _kind, got, _rec = classify_body(pkt_body, state)
            decoded.extend(got)
            if i == 0:
                hdr = unpack_block_header(pkt_body)
                expect(hdr == (7, 0, 8, 12, 0), f"block hdr {hdr}")
        expect(decoded == orig, f"block decode {decoded!r}")

        # Drop 4 data fragments; recover from parity.
        state = FEC.DecodeState()
        decoded = []
        for i, pkt_body in enumerate(pkts):
            if i in (0, 1, 8, 9):
                continue
            _kind, got, rec = classify_body(pkt_body, state)
            decoded.extend(got)
        expect(decoded == orig, f"block recover {decoded!r}")

    tr = SeqTrack()
    expect(tr.add(0, 64) is None, "seq first")
    expect(tr.add(1, 64) is None, "seq next")
    expect(tr.add(4, 64) == "gap 2" and tr.missing == 2, "seq gap")
    expect(tr.add(0, 1400) == "reset" and tr.resets == 1, "seq reset on new phase")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        print(f"self-test: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("self-test: ok")
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Log WInject / bw_test frames from a USB WiFi monitor iface")
    p.add_argument("--iface", default=None, help="monitor interface (default: mon0, else first monitor)")
    p.add_argument("--channel", type=int, default=None, help="set phy channel 1-13 (iw phy set channel)")
    p.add_argument(
        "--bssid",
        default="tunnel",
        help="tunnel | standalone | both | aa:bb:cc:dd:ee:ff (default: tunnel BA:DD:CA:FE:BA:BE)",
    )
    p.add_argument("--quiet", action="store_true", help="stats only, no per-frame lines")
    p.add_argument(
        "--interval",
        type=float,
        default=2.0,
        metavar="SEC",
        help="stats print interval (default 2; 0 disables periodic stats)",
    )
    p.add_argument("--pcap", type=Path, default=None, help="write matching frames as radiotap pcap")
    p.add_argument("--hex", type=int, default=0, metavar="N", help="hex/ascii preview of first N payload bytes")
    p.add_argument("--duration", type=float, default=0.0, help="stop after this many seconds (0 = until Ctrl-C)")
    p.add_argument("--self-test", action="store_true", help="run parser/FEC checks and exit")
    return p.parse_args()


def bssids_from_arg(text: str) -> set[bytes] | None:
    key = text.strip().lower()
    if key in ("tunnel", "bfc", "bfc_tunnel_device"):
        return {BSSID_TUNNEL}
    if key in ("standalone", "winject"):
        return {BSSID_STANDALONE}
    if key in ("both", "all"):
        return {BSSID_TUNNEL, BSSID_STANDALONE}
    if key in ("any", "*"):
        return None
    return {parse_mac(text)}


def main() -> int:
    args = parse_args()
    if args.self_test:
        return self_test()
    if args.channel is not None and not 1 <= args.channel <= 13:
        raise SystemExit("--channel must be 1-13")
    if args.interval < 0:
        raise SystemExit("--interval must be >= 0")

    bssids = bssids_from_arg(args.bssid)
    iface = resolve_iface(args.iface)
    if args.channel is not None:
        set_channel(iface, args.channel)

    sock = open_monitor(iface)
    pcap = PcapWriter(args.pcap) if args.pcap else None
    log = Logger(quiet=args.quiet, hex_bytes=args.hex)
    bssid_label = (
        "any"
        if bssids is None
        else ", ".join(mac_str(b).upper() for b in sorted(bssids))
    )
    phy = wiphy_for(iface) or "?"
    fec_note = "reedsolo" if FEC is not None else "headers only (no reedsolo)"
    print(f"sniff {iface} ({phy})  bssid {bssid_label}  fec {fec_note}")
    if pcap:
        print(f"pcap {pcap.path}")
    print("Ctrl-C to stop")

    deadline = time.time() + args.duration if args.duration > 0 else None
    try:
        while True:
            if deadline is not None and time.time() >= deadline:
                break
            if args.interval > 0 and time.time() - log.last_summary >= args.interval:
                log.summary(final=False)
                log.last_summary = time.time()
            try:
                pkt = sock.recv(65535)
            except socket.timeout:
                continue
            except OSError as err:
                print(f"recv error: {err}", file=sys.stderr)
                break
            log.seen += 1
            rt = parse_radiotap(pkt)
            if rt is None:
                log.bad_rt += 1
                continue
            dot = parse_dot11(pkt[rt.length :], rt.fcs)
            if dot is None:
                log.bad_rt += 1
                continue
            if not dot.is_data:
                log.nondata += 1
                continue
            if bssids is not None and dot.bssid not in bssids:
                log.other_bssid += 1
                continue
            if pcap:
                pcap.write(pkt, time.time())
            log.on_frame(rt, dot)
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()
        if pcap:
            pcap.close()
            print(f"wrote {pcap.packets} packets to {pcap.path}")
        log.summary(final=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
