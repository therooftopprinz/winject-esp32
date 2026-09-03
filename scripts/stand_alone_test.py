#!/usr/bin/env python3
"""WT32-ETH01 STANDALONE two-radio test.

Same UDP-air-UDP path as tools/bw_test.py, but both radios use STANDALONE
mode and operator-assigned airport MACs (Addr2) instead of chip STA
addresses. BSSID is DE:AD:CA:FE:BA:BE.

P2P airports are you|peer. A injects as xx:xx:xx:yy:yy:yy and firmware
on B filters that SA; B injects as yy:yy:yy:xx:xx:xx and A filters the swap.

    host --UDP inject--> A (SA airport-a) --802.11--> B (filter swap(airport-b)=airport-a) --UDP--> host
    host <--UDP--------- A (filter swap(airport-a)=airport-b) <--802.11-- B (SA airport-b) <--UDP-- host

    python3 scripts/stand_alone_test.py
    python3 scripts/stand_alone_test.py --modulation OFDM_24M
    python3 scripts/stand_alone_test.py --all --dual
    python3 scripts/stand_alone_test.py --inject-port 9100
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import bw_test as bw

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

DEFAULT_AIRPORT_A = "AA:BB:CC:22:22:33"
DEFAULT_AIRPORT_B = "22:22:33:AA:BB:CC"
DEFAULT_AIRPORT_A2 = "AA:BB:CC:44:55:66"
DEFAULT_AIRPORT_B2 = "44:55:66:AA:BB:CC"
INJECT_FALLBACK = 9100
INJECT_PORT2 = 9010
HOST_PORT_A2 = 9011
HOST_PORT_B2 = 9012
STANDALONE_BSSID = "DE:AD:CA:FE:BA:BE"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="WT32-ETH01 STANDALONE bandwidth test")
    p.add_argument("--a", default="192.168.253.11", help="radio A Ethernet IP")
    p.add_argument("--b", default="192.168.253.12", help="radio B Ethernet IP")
    p.add_argument("--host", default="", help="host IP that both radios send back to")
    p.add_argument(
        "--airport-a",
        default=DEFAULT_AIRPORT_A,
        help=f"radio A P2P airport you|peer (default {DEFAULT_AIRPORT_A})",
    )
    p.add_argument(
        "--airport-b",
        default=DEFAULT_AIRPORT_B,
        help=f"radio B P2P airport; must be the swap of --airport-a (default {DEFAULT_AIRPORT_B})",
    )
    p.add_argument(
        "--inject-port",
        type=int,
        default=bw.INJECT_PORT,
        help="UDP inject port on both radios (default 9000; 9100 if that bind fails)",
    )
    p.add_argument(
        "--dual",
        action="store_true",
        help="also bind a second airport pair and check mux isolation",
    )
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
    p.add_argument("--all", action="store_true", help="sweep every firmware modulation")
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
    p.add_argument("--skip-mux", action="store_true", help="skip self-loop / dual isolation checks")
    p.add_argument(
        "--restore-tunnel",
        action="store_true",
        help="set_mode BFC_TUNNEL_DEVICE on both radios when the test finishes",
    )
    p.add_argument("--verbose", action="store_true", help="print full console replies")
    return p.parse_args()


def parse_status_mode(text: str) -> str | None:
    raw = bw.parse_status_field(text, "mode")
    return raw.upper() if raw else None


def parse_status_bssid(text: str) -> str | None:
    raw = bw.parse_status_field(text, "bssid")
    return raw.upper() if raw else None


def parse_airport_mac(text: str) -> bytes:
    raw = text.replace(":", "").replace("-", "")
    try:
        mac = bytes.fromhex(raw)
    except ValueError as err:
        raise SystemExit(f"invalid airport MAC {text!r}: {err}") from err
    if len(mac) != 6:
        raise SystemExit(f"airport must be 6 bytes: {text}")
    return mac


def format_airport_mac(mac: bytes) -> str:
    return ":".join(f"{b:02X}" for b in mac)


def swap_airport_mac(mac: bytes) -> bytes:
    return mac[3:] + mac[:3]


def configure_standalone(
    ip: str,
    host: str,
    airport: str,
    inject_port: int,
    tx_port: int,
    extras: list[str],
    quiet: bool,
) -> tuple[bool, str]:
    cmds = [
        "set_mode STANDALONE",
        f"unset_upstream_rx {airport}",
        f"unset_upstream_tx {airport}",
        f"set_upstream_rx {airport} {inject_port}",
        f"set_upstream_tx {airport} {host} {tx_port}",
        *extras,
    ]
    try:
        replies = bw.console(ip, cmds, quiet=quiet)
    except OSError as err:
        return False, str(err)
    if not bw.replies_ok(replies):
        detail = " ".join(text.strip().splitlines()[-1] for text in replies if text.strip())
        return False, detail or "console error"
    return True, ""


def bind_hint(inject_port: int) -> str:
    return (
        f"UDP {inject_port} may still be bound from BFC_TUNNEL_DEVICE; "
        "reboot the radio or pass --inject-port 9100"
    )


def configure_pair(
    args: argparse.Namespace,
    host: str,
    inject_port: int,
    quiet: bool,
) -> bool:
    extras_a: list[str] = []
    extras_b: list[str] = []
    if args.dual:
        extras_a = [
            f"unset_upstream_rx {DEFAULT_AIRPORT_A2}",
            f"unset_upstream_tx {DEFAULT_AIRPORT_A2}",
            f"set_upstream_rx {DEFAULT_AIRPORT_A2} {INJECT_PORT2}",
            f"set_upstream_tx {DEFAULT_AIRPORT_A2} {host} {HOST_PORT_A2}",
        ]
        extras_b = [
            f"unset_upstream_rx {DEFAULT_AIRPORT_B2}",
            f"unset_upstream_tx {DEFAULT_AIRPORT_B2}",
            f"set_upstream_rx {DEFAULT_AIRPORT_B2} {INJECT_PORT2}",
            f"set_upstream_tx {DEFAULT_AIRPORT_B2} {host} {HOST_PORT_B2}",
        ]
    ok_a, err_a = configure_standalone(
        args.a, host, args.airport_a, inject_port, bw.HOST_PORT_A, extras_a, quiet
    )
    ok_b, err_b = configure_standalone(
        args.b, host, args.airport_b, inject_port, bw.HOST_PORT_B, extras_b, quiet
    )
    if ok_a and ok_b:
        return True
    if not ok_a:
        print(f"failed to configure A: {err_a}")
        if "bind" in err_a.lower():
            print(bind_hint(inject_port))
    if not ok_b:
        print(f"failed to configure B: {err_b}")
        if "bind" in err_b.lower():
            print(bind_hint(inject_port))
    return False


def send_count(dest: tuple[str, int], tag: bytes, count: int, size: int, gap: float) -> None:
    bw.send_exact(dest, tag, count, size, gap)


def mux_pass(ok: bool, label: str, detail: str) -> bool:
    print(f"{'PASS' if ok else 'FAIL'}  {label}  {detail}")
    return ok


def run_mux_checks(
    args: argparse.Namespace,
    dest_a: tuple[str, int],
    dest_b: tuple[str, int],
    listen_a: bw.Listener,
    listen_b: bw.Listener,
    dest_a2: tuple[str, int] | None,
    dest_b2: tuple[str, int] | None,
    listen_a2: bw.Listener | None,
    listen_b2: bw.Listener | None,
) -> bool:
    print("\n=== standalone mux ===")
    n = min(10, args.integrity)
    ok = True

    listen_a.stats.clear()
    listen_b.stats.clear()
    send_count(dest_a, b"A", n, 64, 0.03)
    time.sleep(args.drain)
    ok = mux_pass(listen_a.stats.packets == 0, "self-loop A",
                  f"A heard own TX {listen_a.stats.packets} (want 0); "
                  f"B recv {listen_b.stats.packets}/{n}") and ok
    loop_ab = listen_b.stats.packets

    listen_a.stats.clear()
    listen_b.stats.clear()
    send_count(dest_b, b"B", n, 64, 0.03)
    time.sleep(args.drain)
    ok = mux_pass(listen_b.stats.packets == 0, "self-loop B",
                  f"B heard own TX {listen_b.stats.packets} (want 0); "
                  f"A recv {listen_a.stats.packets}/{n}") and ok
    loop_ba = listen_a.stats.packets
    if loop_ab != n or loop_ba != n:
        ok = mux_pass(False, "self-loop forward",
                      f"A->B {loop_ab}/{n}  B->A {loop_ba}/{n}") and ok

    if not args.dual or dest_a2 is None or dest_b2 is None or listen_a2 is None or listen_b2 is None:
        return ok

    listen_a.stats.clear()
    listen_b.stats.clear()
    listen_a2.stats.clear()
    listen_b2.stats.clear()
    send_count(dest_a2, b"C", n, 64, 0.03)
    time.sleep(args.drain)
    leak = listen_a.stats.packets + listen_b.stats.packets
    ok = mux_pass(listen_b2.stats.packets == n, "dual A2->B2",
                  f"{listen_b2.stats.packets}/{n}") and ok
    ok = mux_pass(leak == 0, "isolation pair2->pair1",
                  f"pair1 leak {leak} (want 0)") and ok

    listen_a.stats.clear()
    listen_b.stats.clear()
    listen_a2.stats.clear()
    listen_b2.stats.clear()
    send_count(dest_a, b"A", n, 64, 0.03)
    time.sleep(args.drain)
    leak = listen_a2.stats.packets + listen_b2.stats.packets
    ok = mux_pass(listen_b.stats.packets == n and leak == 0, "isolation pair1->pair2",
                  f"pair1 B recv {listen_b.stats.packets}/{n}  pair2 leak {leak} (want 0)") and ok

    listen_a.stats.clear()
    listen_b.stats.clear()
    listen_a2.stats.clear()
    listen_b2.stats.clear()
    send_count(dest_b2, b"D", n, 64, 0.03)
    time.sleep(args.drain)
    leak = listen_a.stats.packets + listen_b.stats.packets
    ok = mux_pass(listen_a2.stats.packets == n, "dual B2->A2",
                  f"{listen_a2.stats.packets}/{n}") and ok
    ok = mux_pass(leak == 0, "isolation pair2->pair1 (B2)",
                  f"pair1 leak {leak} (want 0)") and ok
    return ok


def main() -> int:
    args = parse_args()
    if args.size < 16 or args.size > bw.MAX_PAYLOAD:
        raise SystemExit(f"--size must be 16..{bw.MAX_PAYLOAD}")
    if args.channel is not None and (args.channel < 1 or args.channel > 13):
        raise SystemExit("--channel must be 1-13")
    if args.all and args.modulation is not None:
        raise SystemExit("use --all or --modulation, not both")
    mac_a = parse_airport_mac(args.airport_a)
    mac_b = parse_airport_mac(args.airport_b)
    if mac_a == mac_b:
        raise SystemExit("--airport-a and --airport-b must differ")
    if swap_airport_mac(mac_a) != mac_b:
        raise SystemExit(
            "--airport-b must be the 3-byte swap of --airport-a "
            f"(want {format_airport_mac(swap_airport_mac(mac_a))})"
        )

    apply_modulation = args.all or args.modulation is not None
    if args.all:
        mods = list(bw.MODULATIONS)
    elif args.modulation is not None:
        mods = bw.parse_modulations(args.modulation)
    else:
        mods = []

    host = args.host or bw.detect_host(args.a)
    quiet = not args.verbose
    cca_label = "enabled" if args.cca else "disabled"
    inject_port = args.inject_port
    print(f"host {host}")
    print(f"mode STANDALONE  airport A {args.airport_a}  airport B {args.airport_b}")

    status_a = bw.log_status(args.a, "A")
    status_b = bw.log_status(args.b, "B")
    display_channel: int | None = args.channel
    if args.channel is None:
        ch_a = bw.parse_status_channel(status_a)
        ch_b = bw.parse_status_channel(status_b)
        display_channel = ch_a
        if ch_a is not None and ch_b is not None and ch_a != ch_b:
            print(f"warning: A is on channel {ch_a}, B is on channel {ch_b}")

    if not apply_modulation:
        mod_a = bw.parse_status_modulation(status_a)
        mod_b = bw.parse_status_modulation(status_b)
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
    if args.dual:
        print(
            f"dual {DEFAULT_AIRPORT_A2}/{DEFAULT_AIRPORT_B2}  "
            f"inject {INJECT_PORT2}  host {HOST_PORT_A2}/{HOST_PORT_B2}"
        )

    if not args.skip_config:
        print("\n=== configure STANDALONE ===")
        configured = configure_pair(args, host, inject_port, quiet)
        inject_explicit = any(
            arg == "--inject-port" or arg.startswith("--inject-port=") for arg in sys.argv
        )
        if not configured and not inject_explicit and inject_port == bw.INJECT_PORT:
            print(f"retrying inject port {INJECT_FALLBACK}")
            inject_port = INJECT_FALLBACK
            configured = configure_pair(args, host, inject_port, quiet)
        if not configured:
            raise SystemExit("failed to configure STANDALONE upstream")
        after_a = bw.log_status(args.a, "A")
        after_b = bw.log_status(args.b, "B")
        for label, text in (("A", after_a), ("B", after_b)):
            mode = parse_status_mode(text)
            bssid = parse_status_bssid(text)
            if mode != "STANDALONE":
                print(f"warning: {label} mode is {mode or '?'} (want STANDALONE)")
            if bssid and bssid != STANDALONE_BSSID:
                print(f"warning: {label} bssid is {bssid} (want {STANDALONE_BSSID})")

    listen_a = bw.Listener(host, bw.HOST_PORT_A)
    listen_b = bw.Listener(host, bw.HOST_PORT_B)
    dest_a = (args.a, inject_port)
    dest_b = (args.b, inject_port)
    listen_a2: bw.Listener | None = None
    listen_b2: bw.Listener | None = None
    dest_a2: tuple[str, int] | None = None
    dest_b2: tuple[str, int] | None = None
    if args.dual:
        listen_a2 = bw.Listener(host, HOST_PORT_A2)
        listen_b2 = bw.Listener(host, HOST_PORT_B2)
        dest_a2 = (args.a, INJECT_PORT2)
        dest_b2 = (args.b, INJECT_PORT2)

    listen_a.start()
    listen_b.start()
    if listen_a2 is not None:
        listen_a2.start()
        listen_b2.start()
    time.sleep(0.2)

    def reset() -> None:
        listen_a.stats.clear()
        listen_b.stats.clear()

    def run_integrity() -> tuple[int, int]:
        reset()
        bw.send_exact(dest_a, b"A", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ab = listen_b.stats.packets
        reset()
        bw.send_exact(dest_b, b"B", args.integrity, 64, 0.03)
        time.sleep(args.drain)
        ba = listen_a.stats.packets
        return ab, ba

    def phase(
        senders: list[tuple[tuple[str, int], bytes, bw.Listener]],
        kbps: float,
    ) -> list[bw.PhaseResult]:
        reset()
        start = time.monotonic()
        results = [0] * len(senders)
        threads: list[threading.Thread] = []

        def run_one(idx: int, dest: tuple[str, int], tag: bytes) -> None:
            results[idx] = bw.send_window(dest, tag, args.size, args.duration, kbps, start)

        for i, (dest, tag, _lis) in enumerate(senders):
            t = threading.Thread(target=run_one, args=(i, dest, tag))
            threads.append(t)
            t.start()
        for t in threads:
            t.join()
        time.sleep(args.drain)
        out: list[bw.PhaseResult] = []
        for i, (_dest, _tag, lis) in enumerate(senders):
            out.append(bw.to_phase(results[i], lis.stats, args.duration))
        return out

    mux_ok = args.skip_mux
    mux_ran = args.skip_mux
    all_results: list[bw.ModResult] = []

    for idx, mod in enumerate(mods, 1):
        offer = args.kbps
        if offer < 0:
            offer = bw.auto_offer_kbps(mod, args.size)
        paced = offer > 0
        print(
            f"\n=== [{idx}/{len(mods)}] ch {channel_label}  {mod}{mod_label_suffix}  "
            f"cca {cca_label}  offer {offer:.0f} kbps ==="
        )
        result = bw.ModResult(modulation=mod, channel=display_channel, offer=offer)

        if not args.skip_config:
            set_mod = mod if apply_modulation else None
            try:
                ok_a = bw.configure_radio(args.a, args.channel, set_mod, args.cca, quiet)
                ok_b = bw.configure_radio(args.b, args.channel, set_mod, args.cca, quiet)
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

        if not mux_ran:
            mux_ok = run_mux_checks(
                args, dest_a, dest_b, listen_a, listen_b, dest_a2, dest_b2, listen_a2, listen_b2
            )
            mux_ran = True

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
        result.uni_ok = bw.loss_ok(result.uni_ab, 5.0, paced) and bw.loss_ok(
            result.uni_ba, 5.0, paced
        )

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
            result.bidir_ok = bw.loss_ok(result.bidir_ab, 10.0, paced) and bw.loss_ok(
                result.bidir_ba, 10.0, paced
            )
        else:
            result.bidir_ok = True

        result.overall = (
            result.config_ok and result.integrity_ok and result.uni_ok and result.bidir_ok
        )
        print("PASS" if result.overall else "FAIL")
        all_results.append(result)

    listen_a.stop()
    listen_b.stop()
    if listen_a2 is not None:
        listen_a2.stop()
        listen_b2.stop()

    print("\n=== result ===")
    paced = args.kbps != 0
    print(bw.fmt_summary(all_results, paced, args.bidir))

    passed = sum(1 for r in all_results if r.overall)
    print(f"\n{passed}/{len(all_results)} modulations PASS  channel {channel_label}")
    if not args.skip_mux:
        print(f"standalone mux  {'PASS' if mux_ok else 'FAIL'}")

    if not args.skip_config:
        if args.restore_tunnel:
            print("\n=== restore BFC_TUNNEL_DEVICE ===")
            try:
                bw.console(args.a, ["set_mode BFC_TUNNEL_DEVICE"], quiet=quiet)
                bw.console(args.b, ["set_mode BFC_TUNNEL_DEVICE"], quiet=quiet)
            except OSError as err:
                print(f"restore failed: {err}")
        else:
            print(
                "\nradios left in STANDALONE; "
                "set_mode BFC_TUNNEL_DEVICE before tools/bw_test.py "
                "(or pass --restore-tunnel)"
            )

    if not mux_ok:
        return 1
    return 0 if passed == len(all_results) and all_results else 1


if __name__ == "__main__":
    raise SystemExit(main())
