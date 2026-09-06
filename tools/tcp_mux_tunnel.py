#!/usr/bin/env python3
"""Multiplex several TCP sockets over one byte pipe (manager TCP forward).

iperf3 opens a control connection plus a data connection to the same host:port.
winject-manager TCP_SERVER_FORWARDING keeps only one client, so the second
connect replaces the first and iperf3 crashes or stalls.

This tool carries many local sockets over a single tunnel connection:

  magic 'MX' | type u8 | conn_id u16be | len u16be | payload

Types: OPEN=1, DATA=2, CLOSE=3.

Mux (client side):  listen for app connects, open one tunnel to the manager.
Demux (server side): accept one tunnel (from manager TCP_CLIENT), dial the
                     real app server once per OPEN.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
from typing import Dict

MAGIC = b"MX"
TYPE_OPEN = 1
TYPE_DATA = 2
TYPE_CLOSE = 3
HDR = struct.Struct("!2sBHH")
MAX_FRAME = 65535


def log(msg: str) -> None:
    print(f"[tcp_mux] {msg}", file=sys.stderr, flush=True)


def set_opts(sock: socket.socket) -> None:
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


def parse_host_port(text: str) -> tuple[str, int]:
    host, _, port_s = text.rpartition(":")
    if not host or not port_s.isdigit():
        raise SystemExit(f"need host:port, got {text!r}")
    return host, int(port_s)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return b""
        buf.extend(chunk)
    return bytes(buf)


def read_frame(sock: socket.socket) -> tuple[int, int, bytes] | None:
    hdr = recv_exact(sock, HDR.size)
    if not hdr:
        return None
    magic, typ, conn_id, length = HDR.unpack(hdr)
    if magic != MAGIC:
        raise OSError(f"bad mux magic {magic!r}")
    if length > MAX_FRAME:
        raise OSError(f"frame length {length} too large")
    payload = b""
    if length:
        payload = recv_exact(sock, length)
        if len(payload) != length:
            return None
    return typ, conn_id, payload


class TunnelIO:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.lock = threading.Lock()

    def send(self, typ: int, conn_id: int, payload: bytes = b"") -> None:
        # DATA may be larger than one frame; OPEN/CLOSE should be empty.
        if typ != TYPE_DATA:
            if len(payload) > MAX_FRAME:
                raise ValueError("frame too large")
            frame = HDR.pack(MAGIC, typ, conn_id, len(payload)) + payload
            with self.lock:
                self.sock.sendall(frame)
            return
        offset = 0
        while offset < len(payload):
            chunk = payload[offset:offset + MAX_FRAME]
            offset += len(chunk)
            frame = HDR.pack(MAGIC, TYPE_DATA, conn_id, len(chunk)) + chunk
            with self.lock:
                self.sock.sendall(frame)


class Mux:
    def __init__(self, listen: str, tunnel: str) -> None:
        self.listen_host, self.listen_port = parse_host_port(listen)
        self.tunnel_host, self.tunnel_port = parse_host_port(tunnel)
        self.tunnel_io: TunnelIO | None = None
        self.tunnel_sock: socket.socket | None = None
        self.conns: Dict[int, socket.socket] = {}
        self.conns_lock = threading.Lock()
        self.next_id = 1
        self.id_lock = threading.Lock()
        self.stop = threading.Event()

    def ensure_tunnel(self) -> TunnelIO:
        if self.tunnel_io is not None:
            return self.tunnel_io
        sock = socket.create_connection((self.tunnel_host, self.tunnel_port), timeout=15)
        set_opts(sock)
        self.tunnel_sock = sock
        self.tunnel_io = TunnelIO(sock)
        threading.Thread(target=self._tunnel_reader, daemon=True).start()
        log(f"tunnel open → {self.tunnel_host}:{self.tunnel_port}")
        return self.tunnel_io

    def _alloc_id(self) -> int:
        with self.id_lock:
            cid = self.next_id
            self.next_id += 1
            return cid

    def _drop_conn(self, conn_id: int, send_close: bool) -> None:
        with self.conns_lock:
            sock = self.conns.pop(conn_id, None)
        if sock is None:
            return
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass
        if send_close and self.tunnel_io is not None and not self.stop.is_set():
            try:
                self.tunnel_io.send(TYPE_CLOSE, conn_id)
            except OSError:
                pass

    def _app_reader(self, conn_id: int, sock: socket.socket) -> None:
        assert self.tunnel_io is not None
        try:
            while not self.stop.is_set():
                data = sock.recv(32768)
                if not data:
                    break
                self.tunnel_io.send(TYPE_DATA, conn_id, data)
        except OSError:
            pass
        self._drop_conn(conn_id, send_close=True)
        log(f"CLOSE id={conn_id} (app EOF)")

    def _tunnel_reader(self) -> None:
        assert self.tunnel_sock is not None
        try:
            while not self.stop.is_set():
                frame = read_frame(self.tunnel_sock)
                if frame is None:
                    break
                typ, conn_id, payload = frame
                if typ == TYPE_DATA:
                    with self.conns_lock:
                        sock = self.conns.get(conn_id)
                    if sock is None:
                        continue
                    try:
                        sock.sendall(payload)
                    except OSError:
                        self._drop_conn(conn_id, send_close=True)
                elif typ == TYPE_CLOSE:
                    self._drop_conn(conn_id, send_close=False)
                    log(f"CLOSE id={conn_id} (peer)")
        except OSError as err:
            log(f"tunnel read error: {err}")
        finally:
            log("tunnel EOF")
            self.stop.set()
            with self.conns_lock:
                ids = list(self.conns)
            for cid in ids:
                self._drop_conn(cid, send_close=False)

    def run(self) -> int:
        listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen_sock.bind((self.listen_host, self.listen_port))
        listen_sock.listen(16)
        log(f"mux listening on {self.listen_host}:{self.listen_port}")
        try:
            while not self.stop.is_set():
                listen_sock.settimeout(0.5)
                try:
                    conn, addr = listen_sock.accept()
                except socket.timeout:
                    continue
                set_opts(conn)
                tunnel = self.ensure_tunnel()
                conn_id = self._alloc_id()
                with self.conns_lock:
                    self.conns[conn_id] = conn
                tunnel.send(TYPE_OPEN, conn_id)
                threading.Thread(
                    target=self._app_reader, args=(conn_id, conn), daemon=True
                ).start()
                log(f"OPEN id={conn_id} from {addr[0]}:{addr[1]}")
        finally:
            self.stop.set()
            listen_sock.close()
            if self.tunnel_sock is not None:
                try:
                    self.tunnel_sock.close()
                except OSError:
                    pass
        return 0


class Demux:
    def __init__(self, listen: str, target: str) -> None:
        self.listen_host, self.listen_port = parse_host_port(listen)
        self.target_host, self.target_port = parse_host_port(target)
        self.stop = threading.Event()

    def serve_tunnel(self, tunnel_sock: socket.socket) -> None:
        set_opts(tunnel_sock)
        tunnel = TunnelIO(tunnel_sock)
        conns: Dict[int, socket.socket] = {}
        lock = threading.Lock()
        local_stop = threading.Event()

        def drop(conn_id: int, send_close: bool) -> None:
            with lock:
                sock = conns.pop(conn_id, None)
            if sock is None:
                return
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
            if send_close and not local_stop.is_set():
                try:
                    tunnel.send(TYPE_CLOSE, conn_id)
                except OSError:
                    pass

        def app_reader(conn_id: int, sock: socket.socket) -> None:
            try:
                while not local_stop.is_set():
                    data = sock.recv(32768)
                    if not data:
                        break
                    tunnel.send(TYPE_DATA, conn_id, data)
            except OSError:
                pass
            drop(conn_id, send_close=True)
            log(f"CLOSE id={conn_id} (target EOF)")

        def on_open(conn_id: int) -> None:
            drop(conn_id, send_close=False)
            try:
                sock = socket.create_connection(
                    (self.target_host, self.target_port), timeout=5
                )
            except OSError as err:
                log(
                    f"OPEN id={conn_id} dial "
                    f"{self.target_host}:{self.target_port} failed: {err}"
                )
                try:
                    tunnel.send(TYPE_CLOSE, conn_id)
                except OSError:
                    pass
                return
            set_opts(sock)
            with lock:
                conns[conn_id] = sock
            threading.Thread(
                target=app_reader, args=(conn_id, sock), daemon=True
            ).start()
            log(f"OPEN id={conn_id} → {self.target_host}:{self.target_port}")

        log("demux tunnel accepted")
        try:
            while not local_stop.is_set():
                frame = read_frame(tunnel_sock)
                if frame is None:
                    break
                typ, conn_id, payload = frame
                if typ == TYPE_OPEN:
                    on_open(conn_id)
                elif typ == TYPE_DATA:
                    with lock:
                        sock = conns.get(conn_id)
                    if sock is None:
                        continue
                    try:
                        sock.sendall(payload)
                    except OSError:
                        drop(conn_id, send_close=True)
                elif typ == TYPE_CLOSE:
                    drop(conn_id, send_close=False)
                    log(f"CLOSE id={conn_id} (peer)")
        except OSError as err:
            log(f"tunnel read error: {err}")
        finally:
            local_stop.set()
            with lock:
                ids = list(conns)
            for cid in ids:
                drop(cid, send_close=False)
            try:
                tunnel_sock.close()
            except OSError:
                pass
            log("tunnel EOF")

    def run(self) -> int:
        listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen_sock.bind((self.listen_host, self.listen_port))
        listen_sock.listen(1)
        log(
            f"demux listening on {self.listen_host}:{self.listen_port} "
            f"→ {self.target_host}:{self.target_port}"
        )
        try:
            while not self.stop.is_set():
                listen_sock.settimeout(0.5)
                try:
                    tunnel, addr = listen_sock.accept()
                except socket.timeout:
                    continue
                log(f"tunnel from {addr[0]}:{addr[1]}")
                self.serve_tunnel(tunnel)
                log("waiting for next tunnel")
        finally:
            listen_sock.close()
        return 0


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--mux", action="store_true", help="client-side multiplexer")
    g.add_argument("--demux", action="store_true", help="server-side demultiplexer")
    p.add_argument("--listen", required=True, help="host:port to bind")
    p.add_argument("--tunnel", help="mux: manager TCP_SERVER host:port")
    p.add_argument("--target", help="demux: real server host:port")
    args = p.parse_args()
    if args.mux:
        if not args.tunnel:
            raise SystemExit("--mux requires --tunnel host:port")
        return Mux(args.listen, args.tunnel).run()
    if not args.target:
        raise SystemExit("--demux requires --target host:port")
    return Demux(args.listen, args.target).run()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
