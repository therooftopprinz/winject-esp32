"""Wait for a winject radio to come back after reboot/OTA via ping/pong.

No long static sleeps: poll UDP console `ping` until we see a missed reply
(down), then until `pong` returns (up). Deadline after down is 10s by default.
"""

from __future__ import annotations

import socket
import time
from typing import Optional


def ping_ok(ip: str, *, port: int = 22, timeout: float = 0.35) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b"ping\n", (ip, port))
        return b"pong" in s.recvfrom(64)[0]
    except OSError:
        return False
    finally:
        s.close()


def wait_reboot_pong(
    ip: str,
    *,
    label: str = "radio",
    port: int = 22,
    overall_s: float = 45.0,
    after_down_s: float = 10.0,
    down_grace_s: float = 8.0,
    poll_s: float = 0.15,
    ping_timeout: float = 0.35,
    log: bool = True,
) -> bool:
    """Return True once the radio has gone down then answered pong again.

    Requires a missed ping first so a stale pre-reboot pong is not treated as
    'back'. After the first miss, fail if no pong within after_down_s.

    If the radio never drops within down_grace_s (e.g. set_mode no-op), return
    True immediately — caller already has a live pong.
    """
    t0 = time.monotonic()
    seen_down = False
    down_at: Optional[float] = None
    while time.monotonic() - t0 < overall_s:
        ok = ping_ok(ip, port=port, timeout=ping_timeout)
        if not seen_down:
            if not ok:
                seen_down = True
                down_at = time.monotonic()
                if log:
                    print(f"{label} down", flush=True)
            elif time.monotonic() - t0 >= down_grace_s:
                if log:
                    print(f"{label} still up (no reboot)", flush=True)
                return True
            time.sleep(poll_s)
            continue
        if ok:
            if log:
                print(f"{label} pong", flush=True)
            return True
        assert down_at is not None
        if time.monotonic() - down_at > after_down_s:
            if log:
                print(f"{label} timeout after down ({after_down_s:.0f}s)", flush=True)
            return False
        time.sleep(poll_s)
    if log:
        print(f"{label} timeout overall ({overall_s:.0f}s)", flush=True)
    return False


def wait_pong(
    ip: str,
    *,
    label: str = "radio",
    port: int = 22,
    deadline_s: float = 10.0,
    ping_timeout: float = 0.25,
    log: bool = True,
) -> bool:
    """Poll ping until pong (no down requirement). Prefer wait_reboot_pong after OTA."""
    t_end = time.monotonic() + deadline_s
    while time.monotonic() < t_end:
        if ping_ok(ip, port=port, timeout=ping_timeout):
            if log:
                print(f"{label} pong", flush=True)
            return True
    if log:
        print(f"{label} timeout ({deadline_s:.0f}s)", flush=True)
    return False
