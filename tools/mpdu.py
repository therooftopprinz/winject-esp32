#!/usr/bin/env python3
"""802.11 MPDU pack/unpack for winject host tools (matches firmware frame packing)."""

from __future__ import annotations

WIFI_HDR_LEN = 24
WIFI_PDU_SLOTS = 5
WIFI_PAYLOAD_MAX = 1476
PREFIX_TUNNEL = bytes((0xBA, 0xDD, 0xCA, 0xFE))
PREFIX_STANDALONE = bytes((0xCA, 0xFE, 0xBA, 0xBE))

_SEQ = 0


def _emit_bits(packed: bytearray, bit: list[int], value: int, nbits: int) -> None:
    for i in range(nbits):
        if value & (1 << i):
            byte_i = bit[0] // 8
            bit_i = bit[0] % 8
            packed[byte_i] |= 1 << bit_i
        bit[0] += 1


def _take_bits(packed: bytes, bit: list[int], nbits: int) -> int:
    value = 0
    for i in range(nbits):
        byte_i = bit[0] // 8
        bit_i = bit[0] % 8
        if packed[byte_i] & (1 << bit_i):
            value |= 1 << i
        bit[0] += 1
    return value


def pack_slots(slots: list[tuple[int, int]]) -> tuple[bytes, bytes]:
    packed = bytearray(12)
    bit = [0]
    _emit_bits(packed, bit, 1, 1)  # ig
    for i in range(WIFI_PDU_SLOTS):
        bus, size = slots[i] if i < len(slots) else (0, 0)
        _emit_bits(packed, bit, bus & 0xFF, 8)
        _emit_bits(packed, bit, size & 0x7FF, 11)
    return bytes(packed[0:6]), bytes(packed[6:12])


def unpack_slots(addr1: bytes, addr2: bytes) -> list[tuple[int, int]]:
    packed = bytes(addr1) + bytes(addr2)
    bit = [0]
    _take_bits(packed, bit, 1)  # skip ig
    slots: list[tuple[int, int]] = []
    for _ in range(WIFI_PDU_SLOTS):
        bus = _take_bits(packed, bit, 8)
        size = _take_bits(packed, bit, 11)
        slots.append((bus, size))
    return slots


def mode_prefix(mode: str) -> bytes:
    name = mode.strip().upper()
    if name == "STANDALONE":
        return PREFIX_STANDALONE
    return PREFIX_TUNNEL


def build_mpdu(
    body: bytes,
    bus: int,
    domain: int,
    mode: str = "BFC_TUNNEL_DEVICE",
) -> bytes:
    global _SEQ
    if bus < 1 or bus > 255:
        raise ValueError(f"bus must be 1..255, got {bus}")
    if domain < 1 or domain > 65535:
        raise ValueError(f"domain must be 1..65535, got {domain}")
    if not body or len(body) > WIFI_PAYLOAD_MAX:
        raise ValueError(f"body length {len(body)} out of range")
    slots = [(bus, len(body))]
    addr1, addr2 = pack_slots(slots)
    prefix = mode_prefix(mode)
    addr3 = prefix + bytes(((domain >> 8) & 0xFF, domain & 0xFF))
    seq = _SEQ & 0x0FFF
    _SEQ = (_SEQ + 1) & 0xFFFF
    seq_ctl = (seq << 4) & 0xFFFF
    hdr = bytearray(WIFI_HDR_LEN)
    hdr[0] = 0x08
    hdr[1] = 0x00
    hdr[4:10] = addr1
    hdr[10:16] = addr2
    hdr[16:22] = addr3
    hdr[22] = seq_ctl & 0xFF
    hdr[23] = (seq_ctl >> 8) & 0xFF
    return bytes(hdr) + body


def unpack_mpdu(mpdu: bytes) -> list[tuple[int, bytes]]:
    if len(mpdu) < WIFI_HDR_LEN:
        return []
    slots = unpack_slots(mpdu[4:10], mpdu[10:16])
    body = mpdu[WIFI_HDR_LEN:]
    if sum(size for _, size in slots) != len(body):
        return []
    out: list[tuple[int, bytes]] = []
    off = 0
    for bus, size in slots:
        if size == 0:
            continue
        if size > WIFI_PAYLOAD_MAX or off + size > len(body):
            return []
        out.append((bus, body[off : off + size]))
        off += size
    return out


def parse_bus_int(text: str) -> int:
    s = text.strip().lower()
    if s.startswith("bus="):
        s = s[4:]
    if s.startswith("0x"):
        s = s[2:]
    value = int(s, 16)
    if not 1 <= value <= 255:
        raise ValueError(f"bus must be 1..ff: {text}")
    return value


def parse_domain_int(text: str | int) -> int:
    if isinstance(text, int):
        n = text
    else:
        s = text.strip().lower()
        if s.startswith("0x"):
            s = s[2:]
        n = int(s, 16)
    if not 1 <= n <= 65535:
        raise ValueError(f"domain must be 1..ffff: {text}")
    return n
