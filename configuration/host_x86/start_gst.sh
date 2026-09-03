#!/usr/bin/env bash
# x86 video sink: receive H264 from winject-manager and display.
# TRANSPORT=udp: udpsrc + RTP depay → UDP_CLIENT_FORWARDING (upstream-0)
# TRANSPORT=tcp: udpsrc + MPEG-TS   → UDP_CLIENT_FORWARDING (upstream-1, --tcp-gst)
# Defaults live in gst.env; override any var in the environment.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=gst.env
set -a
# shellcheck disable=SC1091
source "${GST_ENV:-$HERE/gst.env}"
set +a

TRANSPORT="${TRANSPORT:-udp}"
HOST="${HOST:-127.0.0.1}"
SINK="${SINK:-ximagesink}"
CAPS="${GST_CAPS:-application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96}"
UDP_PORT="${UDP_PORT:-21082}"
TCP_PORT="${TCP_PORT:-21083}"

case "$TRANSPORT" in
    udp) PORT="${PORT:-$UDP_PORT}" ;;
    tcp) PORT="${PORT:-$TCP_PORT}" ;;
    *)
        echo "TRANSPORT must be udp or tcp (got ${TRANSPORT})" >&2
        exit 1
        ;;
esac

echo "GStreamer ${TRANSPORT} :${PORT} -> ${SINK}" >&2

case "$TRANSPORT" in
    udp)
        exec gst-launch-1.0 -e \
            udpsrc port="${PORT}" buffer-size=2097152 caps="${CAPS}" \
            ! rtph264depay \
            ! avdec_h264 \
            ! videoconvert \
            ! "${SINK}" sync=false
        ;;
    tcp)
        exec gst-launch-1.0 -e \
            udpsrc port="${PORT}" buffer-size=2097152 \
            ! queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 \
            ! tsdemux ignore-pcr=true \
            ! h264parse \
            ! avdec_h264 \
            ! videoconvert \
            ! "${SINK}" sync=false
        ;;
esac
