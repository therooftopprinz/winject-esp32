#!/usr/bin/env python3
"""Listen for WInject UDP logger lines.

On the radio:
  set_logger address=<this-host>:<port> level=warn
  unset_logger

Example:
  python3 tools/udp_log.py --port 9999
"""

from __future__ import annotations

import argparse
import socket
import sys


def main() -> int:
    p = argparse.ArgumentParser(description="WInject UDP logger listener")
    p.add_argument("--host", default="0.0.0.0", help="bind address")
    p.add_argument("--port", type=int, default=9999, help="UDP port")
    args = p.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    print(f"listening udp {args.host}:{args.port}", flush=True)
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            line = data.decode("utf-8", "replace").rstrip("\n")
            print(f"{addr[0]}:{addr[1]}  {line}", flush=True)
    except KeyboardInterrupt:
        print("\nstopped", flush=True)
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
