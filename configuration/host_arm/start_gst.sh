#!/usr/bin/env bash
# ARM camera source: capture UVC, encode H264, send to winject-manager.
# TRANSPORT=udp: RTP + udpsink     → UDP_SERVER_FORWARDING (upstream-0)
# TRANSPORT=tcp: MPEG-TS + udpsink → UDP_SERVER_FORWARDING (upstream-1, --tcp-gst)
# Defaults live in gst.env; override any var in the environment.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=gst.env
set -a
# shellcheck disable=SC1091
source "${GST_ENV:-$HERE/gst.env}"
set +a

TRANSPORT="${TRANSPORT:-udp}"
DEVICE="${DEVICE:-/dev/video0}"
HOST="${HOST:-127.0.0.1}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FRAMERATE="${FRAMERATE:-30/1}"
BITRATE="${BITRATE:-10000}"
GOP="${GOP:-30}"
FORMAT="${FORMAT:-auto}"
MTU="${MTU:-1200}"
UDP_PORT="${UDP_PORT:-22081}"
TCP_PORT="${TCP_PORT:-22082}"

case "$TRANSPORT" in
    udp) PORT="${PORT:-$UDP_PORT}" ;;
    tcp) PORT="${PORT:-$TCP_PORT}" ;;
    *)
        echo "TRANSPORT must be udp or tcp (got ${TRANSPORT})" >&2
        exit 1
        ;;
esac

gst_has() {
    gst-inspect-1.0 "$1" >/dev/null 2>&1
}

pick_encoder() {
    if [[ -n "${ENC:-}" ]]; then
        echo "$ENC"
        return
    fi
    local candidate
    for candidate in mpph264enc x264enc openh264enc; do
        if gst_has "$candidate"; then
            echo "$candidate"
            return
        fi
    done
    echo "no H264 encoder found (install gstreamer1.0-plugins-ugly for x264enc)" >&2
    exit 1
}

pick_jpegdec() {
    if [[ -n "${JPEGDEC:-}" ]]; then
        echo "$JPEGDEC"
        return
    fi
    local candidate
    for candidate in mppjpegdec jpegdec; do
        if gst_has "$candidate"; then
            echo "$candidate"
            return
        fi
    done
    echo "no JPEG decoder found (needed for FORMAT=mjpeg)" >&2
    exit 1
}

detect_format() {
    local listing
    listing="$(v4l2-ctl --device="${DEVICE}" --list-formats-ext 2>/dev/null || true)"
    if [[ -z "$listing" ]]; then
        echo mjpeg
        return
    fi
    if awk -v w="${WIDTH}" -v h="${HEIGHT}" '
        /^\t\[[0-9]+\]: / {
            in_mjpg = ($0 ~ /MJPG/)
            next
        }
        in_mjpg && $0 ~ /Size: Discrete / {
            if ($3 == w "x" h) {
                found = 1
                exit
            }
        }
        END { exit found ? 0 : 1 }
    ' <<<"$listing"; then
        echo mjpeg
    else
        echo raw
    fi
}

if [[ "$FORMAT" == auto ]]; then
    FORMAT="$(detect_format)"
fi

ENCODER="$(pick_encoder)"

case "$ENCODER" in
    x264enc)
        ENCODER_ARGS=(
            tune=zerolatency
            speed-preset=ultrafast
            "bitrate=${BITRATE}"
            "key-int-max=${GOP}"
        )
        ;;
    openh264enc)
        ENCODER_ARGS=("bitrate=$((BITRATE * 1000))" complexity=low)
        ;;
    mpph264enc)
        ENCODER_ARGS=("bps=$((BITRATE * 1000))" "gop=${GOP}" profile=high)
        ;;
    *)
        ENCODER_ARGS=()
        ;;
esac

cmd=(gst-launch-1.0 -e v4l2src "device=${DEVICE}" do-timestamp=true)
case "$FORMAT" in
    mjpeg)
        JPEGDEC="$(pick_jpegdec)"
        echo "UVC ${DEVICE} ${WIDTH}x${HEIGHT}@${FRAMERATE} FORMAT=mjpeg ${JPEGDEC} -> ${ENCODER} ${BITRATE}kbps ${TRANSPORT}://${HOST}:${PORT}" >&2
        cmd+=(! "image/jpeg,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}")
        if gst_has jpegparse; then
            cmd+=(! jpegparse)
        fi
        cmd+=(! "${JPEGDEC}" ! videoconvert ! "video/x-raw,format=NV12")
        ;;
    raw)
        echo "UVC ${DEVICE} ${WIDTH}x${HEIGHT}@${FRAMERATE} FORMAT=raw -> ${ENCODER} ${BITRATE}kbps ${TRANSPORT}://${HOST}:${PORT}" >&2
        cmd+=(
            ! "video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FRAMERATE}"
            ! videoconvert
            ! "video/x-raw,format=NV12"
        )
        ;;
    *)
        echo "FORMAT must be auto, mjpeg, or raw (got ${FORMAT})" >&2
        exit 1
        ;;
esac

cmd+=(! queue max-size-buffers=4 leaky=downstream)
if ((${#ENCODER_ARGS[@]})); then
    cmd+=(! "${ENCODER}" "${ENCODER_ARGS[@]}")
else
    cmd+=(! "${ENCODER}")
fi
if gst_has h264parse; then
    cmd+=(! h264parse config-interval=-1)
fi

case "$TRANSPORT" in
    udp)
        # RTP MTU stays under the radio UDP payload max (1476).
        cmd+=(
            ! rtph264pay "mtu=${MTU}" config-interval=1 pt=96
            ! udpsink "host=${HOST}" "port=${PORT}" sync=false async=false
        )
        ;;
    tcp)
        if ! gst_has mpegtsmux; then
            echo "mpegtsmux required for TRANSPORT=tcp" >&2
            exit 1
        fi
        cmd+=(
            ! "video/x-h264,stream-format=byte-stream"
            ! mpegtsmux alignment=7
            ! queue max-size-buffers=8 max-size-time=0 max-size-bytes=0
            ! udpsink "host=${HOST}" "port=${PORT}" sync=false async=false
        )
        ;;
esac

exec "${cmd[@]}"
