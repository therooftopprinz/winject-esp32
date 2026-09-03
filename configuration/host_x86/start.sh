#!/usr/bin/env bash
# x86 video sink (display): winject-manager + GStreamer receive.
# Default: RTP/UDP.  ./start.sh --tcp-gst  → MPEG-TS/UDP (see gst.env ports).
# Both manager upstreams are UDP_CLIENT; gst (udpsrc) binds before manager.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CONFIG="${CONFIG:-$HERE/config.cfg}"
TRANSPORT=udp

for arg in "$@"; do
    case "$arg" in
        --tcp-gst) TRANSPORT=tcp ;;
        -h|--help)
            echo "Usage: $0 [--tcp-gst]" >&2
            exit 0
            ;;
        *)
            echo "unknown arg: $arg (try --tcp-gst)" >&2
            exit 1
            ;;
    esac
done

# shellcheck source=gst.env
set -a
# shellcheck disable=SC1091
source "${GST_ENV:-$HERE/gst.env}"
set +a

if [[ "$TRANSPORT" == tcp ]]; then
    PORT="${PORT:-${TCP_PORT:-21083}}"
else
    PORT="${PORT:-${UDP_PORT:-21082}}"
fi
PROTO=UDP
SS_FLAG=-uln

# shellcheck source=../../scripts/ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"
ensure_winject_manager "$ROOT" build_manager_x86

pkill -f "winject-manager.*config.cfg" 2>/dev/null || true
pkill -x winject-manager 2>/dev/null || true
sleep 0.2

cleanup() {
    # Manager first so it can inject TCP CLOSE before the sockets vanish.
    if [[ -n "${MGR_PID:-}" ]]; then
        kill -TERM "$MGR_PID" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$MGR_PID" 2>/dev/null || break
            sleep 0.05
        done
        kill -KILL "$MGR_PID" 2>/dev/null || true
        wait "$MGR_PID" 2>/dev/null || true
        MGR_PID=
    fi
    if [[ -n "${GST_PID:-}" ]]; then
        kill "$GST_PID" 2>/dev/null || true
        wait "$GST_PID" 2>/dev/null || true
        GST_PID=
    fi
}
trap cleanup EXIT INT TERM

# CLIENT modes: gst must bind before manager connects.
TRANSPORT="$TRANSPORT" "$HERE/start_gst.sh" &
GST_PID=$!

ready=0
for _ in $(seq 1 50); do
    if ! kill -0 "$GST_PID" 2>/dev/null; then
        wait "$GST_PID" || true
        echo "GStreamer exited before binding ${PROTO} ${PORT}" >&2
        exit 1
    fi
    if ss "$SS_FLAG" 2>/dev/null | grep -qE ":${PORT}([[:space:]]|$)"; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
    echo "GStreamer did not bind ${PROTO} ${PORT} in time" >&2
    exit 1
fi

"$MANAGER" "$CONFIG" &
MGR_PID=$!

wait -n "$MGR_PID" "$GST_PID"
exit $?
