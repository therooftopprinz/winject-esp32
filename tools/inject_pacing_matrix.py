#!/usr/bin/env python3
"""Sweep manager max_data_per_tick and radio .9 inject_tune; A→B goodput each cell.

Example:
  python3 tools/inject_pacing_matrix.py --a 192.168.253.9 --b 192.168.253.14
"""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cons(ip: str, cmd: str, timeout: float = 5.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(cmd.encode(), (ip, 22))
    chunks: list[bytes] = []
    while True:
        try:
            d, _ = s.recvfrom(65535)
            chunks.append(d)
            if len(d) < 8000:
                break
        except socket.timeout:
            break
    s.close()
    return b"".join(chunks).decode(errors="replace")


def inject_tune(ip: str, flush_batch: int, emac_gap: int) -> None:
    cmd = (
        f"set_inject_tune flush_batch={flush_batch} emac_gap_ticks={emac_gap} "
        "max_in_flight=6 staging_margin=4\n"
    )
    reply = cons(ip, cmd)
    if "ok" not in reply.splitlines()[0].lower() and "ok" not in reply:
        raise RuntimeError(f"{ip} inject_tune: {reply[:200]}")


def grab_us(text: str, key: str) -> int | None:
    m = re.search(rf"{re.escape(key)}=(\d+)us", text)
    return int(m.group(1)) if m else None


@dataclass
class Cell:
    mgr_tick: int
    flush_batch: int
    emac_gap: int
    kbps: float = 0.0
    loss_pct: float = 0.0
    inject_wait_us: int | None = None


def patch_conf(src: Path, dst: Path, device: str, host: str, mgr_tick: int) -> None:
    text = src.read_text()
    text = re.sub(
        r"^winject\.device.*",
        f"winject.device        = {device}",
        text,
        count=1,
        flags=re.M,
    )
    text = re.sub(
        r"^winject\.local_ip.*",
        f"winject.local_ip      = {host}",
        text,
        count=1,
        flags=re.M,
    )
    if re.search(r"^winject\.max_data_per_tick", text, re.M):
        text = re.sub(
            r"^winject\.max_data_per_tick.*",
            f"winject.max_data_per_tick = {mgr_tick}",
            text,
            count=1,
            flags=re.M,
        )
    else:
        text += f"\nwinject.max_data_per_tick = {mgr_tick}\n"
    dst.write_text(text)


def stop_managers() -> None:
    subprocess.run(["pkill", "-f", "winject-manager"], check=False)
    for _ in range(40):
        r = subprocess.run(["pgrep", "-f", "winject-manager"], capture_output=True)
        if r.returncode != 0:
            return
        time.sleep(0.25)
    subprocess.run(["pkill", "-9", "-f", "winject-manager"], check=False)
    time.sleep(0.5)


def run_ab(
    radio_a: str,
    radio_b: str,
    host: str,
    conf_a: Path,
    conf_b: Path,
    manager: Path,
) -> tuple[float, float]:
    log_dir = Path(tempfile.mkdtemp(prefix="pace_mtx_"))
    stop_managers()
    ma = subprocess.Popen(
        [str(manager), str(conf_a)],
        stdout=open(log_dir / "a.log", "w"),
        stderr=subprocess.STDOUT,
    )
    mb = subprocess.Popen(
        [str(manager), str(conf_b)],
        stdout=open(log_dir / "b.log", "w"),
        stderr=subprocess.STDOUT,
    )
    deadline = time.time() + 30
    while time.time() < deadline:
        la = (log_dir / "a.log").read_text()
        lb = (log_dir / "b.log").read_text()
        if "manager running" in la and "manager running" in lb:
            break
        time.sleep(0.25)
    else:
        raise RuntimeError(
            f"managers failed to start; tail A:\n{la[-800:]}\nB:\n{lb[-800:]}"
        )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/configure_manager_ci.py"),
            "--radio",
            radio_a,
            "--host",
            host,
            "--log",
            str(log_dir / "a.log"),
            "--quiet",
        ],
        check=False,
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/configure_manager_ci.py"),
            "--radio",
            radio_b,
            "--host",
            host,
            "--log",
            str(log_dir / "b.log"),
            "--quiet",
        ],
        check=False,
    )
    bw_log = log_dir / "bw.log"
    with bw_log.open("w") as bw_out:
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/bw_test.py"),
                "--udp",
                "--a",
                radio_a,
                "--b",
                radio_b,
                "--host",
                host,
                "--no-cca",
                "--channel",
                "1",
                "--modulation",
                "OFDM_24M",
                "--test-ab",
            ],
            stdout=bw_out,
            stderr=subprocess.STDOUT,
            text=True,
        )
    for proc in (ma, mb):
        proc.terminate()
    for proc in (ma, mb):
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    stop_managers()
    out = bw_log.read_text()
    m = re.search(
        r"A->B sent \d+ recv \d+\s+([\d.]+) kbps\s+loss ([\d.]+)%",
        out,
    )
    kbps, loss = (float(m.group(1)), float(m.group(2))) if m else (0.0, 0.0)
    return kbps, loss


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument(
        "--manager",
        default=str(ROOT / "build_manager_arm/winject-manager"),
    )
    p.add_argument("--quick", action="store_true", help="corners only (4 runs)")
    args = p.parse_args()
    manager = Path(args.manager)
    if not manager.is_file():
        print(f"build manager first: {manager}", file=sys.stderr)
        return 1

    def prepare_bench() -> None:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts/prepare_radios_for_manager.py"),
                "--a",
                args.a,
                "--b",
                args.b,
                "--host",
                args.host,
                "--channel",
                "1",
                "--modulation",
                "OFDM_24M",
                "--no-cca",
            ],
            check=True,
        )
        time.sleep(8)

    if args.quick:
        grid = [
            (4, 8, 0),
            (1, 8, 0),
            (4, 1, 0),
            (4, 1, 1),
        ]
    else:
        # Baseline first, then other corners.
        grid = [
            (4, 8, 0),
            (1, 8, 0),
            (4, 1, 0),
            (4, 1, 1),
            (1, 1, 0),
            (1, 1, 1),
            (1, 8, 1),
            (4, 8, 1),
        ]

    conf_a_src = ROOT / "configuration/winject-tests/lat_udp_a.cfg"
    conf_b_src = ROOT / "configuration/winject-tests/lat_udp_b.cfg"
    work = Path(tempfile.mkdtemp(prefix="pace_mtx_conf_"))
    results: list[Cell] = []

    print(
        f"inject pacing matrix A→B  A={args.a} B={args.b}  "
        f"({len(grid)} cells)\n"
    )
    print(
        f"{'mgr/tick':>8} {'flush':>5} {'gap':>3} "
        f"{'kbps':>8} {'loss%':>6} {'inj_wait':>8}"
    )
    print("-" * 48)

    for mgr_tick, flush_batch, emac_gap in grid:
        prepare_bench()
        inject_tune(args.a, flush_batch, emac_gap)
        patch_conf(conf_a_src, work / "a.conf", args.a, args.host, mgr_tick)
        patch_conf(conf_b_src, work / "b.conf", args.b, args.host, 4)
        kbps, loss = run_ab(
            args.a, args.b, args.host, work / "a.conf", work / "b.conf", manager
        )
        st = cons(args.a, "status\n")
        tx = next((ln for ln in st.splitlines() if ln.startswith("channel_tx")), "")
        iw = grab_us(tx, "inject_wait")
        cell = Cell(mgr_tick, flush_batch, emac_gap, kbps, loss, iw)
        results.append(cell)
        iw_s = str(iw) if iw is not None else "-"
        print(
            f"{mgr_tick:8d} {flush_batch:5d} {emac_gap:3d} "
            f"{kbps:8.1f} {loss:6.1f} {iw_s:>8}"
        )
        time.sleep(2)

    # restore bench defaults on .9
    inject_tune(args.a, 8, 0)

    best = max(results, key=lambda c: c.kbps)
    print(
        f"\nbest A→B: {best.kbps:.1f} kbps loss {best.loss_pct:.1f}% "
        f"(mgr_tick={best.mgr_tick} flush={best.flush_batch} gap={best.emac_gap})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
