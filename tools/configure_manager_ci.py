#!/usr/bin/env python3
"""Point a radio at a running manager's channel_info UDP port (skip_console runs)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import bw_test as bw  # noqa: E402


def clear_ci_subscribers(radio: str, quiet: bool) -> None:
    replies = bw.console(radio, ["status"], quiet=True)
    text = "\n".join(replies)
    for line in text.splitlines():
        if "channel_info subscribers=" not in line:
            continue
        rest = line.split("=", 1)[1].strip()
        if not rest:
            return
        for entry in rest.split(","):
            entry = entry.strip()
            if not entry:
                continue
            bw.console(radio, [f"unset_upstream_ci to={entry}"], quiet=quiet)


def ci_port_from_log(path: Path) -> int:
    text = path.read_text(errors="replace")
    m = re.search(r"channel_info listening on udp (\d+)", text)
    if not m:
        raise SystemExit(f"no channel_info port in {path}")
    return int(m.group(1))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--radio", required=True, help="radio IP (console UDP :22)")
    p.add_argument("--host", required=True, help="manager host IP (local_ip)")
    p.add_argument("--log", required=True, type=Path, help="manager log file")
    p.add_argument("--quiet", action="store_true")
    args = p.parse_args()
    clear_ci_subscribers(args.radio, args.quiet)
    port = ci_port_from_log(args.log)
    cmd = f"set_upstream_ci to={args.host}:{port}"
    replies = bw.console(args.radio, [cmd], quiet=args.quiet)
    if not bw.replies_ok(replies):
        print(f"{args.radio}: {cmd} failed")
        return 1
    if not args.quiet:
        print(f"{args.radio}: {cmd} ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
