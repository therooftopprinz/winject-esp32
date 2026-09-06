#!/usr/bin/env bash
# Start winject-manager on both radios and measure TCP goodput with iperf3
# through manager TCP forwarding (same ports as manager_tcp_bw_test.sh).
#
# Why mux/demux: iperf3 opens a control socket + a data socket. The manager
# TCP_SERVER keeps only one client (second connect replaces the first →
# segfault/stall). tools/tcp_mux_tunnel.py carries both sockets over the
# single manager byte pipe.
#
# Topology (A→B):
#   iperf3 -c :15201 → mux → manager A :29000 → air
#     → manager B TCP_CLIENT → demux :9002 → iperf3 -s :15202
#
# Requires: iperf3 on PATH.
#
# Usage:
#   ./scripts/manager_iperf3_bw_test.sh
#   ./scripts/manager_iperf3_bw_test.sh --a 192.168.127.181 --b 192.168.128.119
#   ./scripts/manager_iperf3_bw_test.sh --dir ab --time 20 --bitrate 8M
#   ./scripts/manager_iperf3_bw_test.sh --dir both --no-cca
#   ./scripts/manager_iperf3_bw_test.sh 192.168.253.11 192.168.253.12 192.168.253.106
#   ./scripts/manager_iperf3_bw_test.sh -- --parallel 2 -i 1

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"
MUX_TOOL="$ROOT/tools/tcp_mux_tunnel.py"
CONF_A="$ROOT/configuration/winject-tests/bw_a.cfg"
CONF_B="$ROOT/configuration/winject-tests/bw_b.cfg"
LOG_DIR="${TMPDIR:-/tmp}/winject-iperf3-$$"
mkdir -p "$LOG_DIR"

RADIO_A="192.168.253.11"
RADIO_B="192.168.253.12"
HOST_IP="192.168.253.106"
HOST_SET=0
PREP_EXTRA=()

# Ports must match configuration/winject-tests/bw_{a,b}.cfg
PORT_SEND_AB=29000   # manager A TCP_SERVER
PORT_SEND_BA=29001   # manager B TCP_SERVER
PORT_RECV_AB=9002    # manager B TCP_CLIENT → demux
PORT_RECV_BA=9001    # manager A TCP_CLIENT → demux

# Local iperf3 / mux ports (stay on this host; not forwarded by the radio)
MUX_AB=15201
IPERF_AB=15202
MUX_BA=15211
IPERF_BA=15212

DIR="both"           # ab | ba | both | bidir
TIME=10
BITRATE=""           # empty = iperf3 default (unlimited TCP)
REVERSE=0
PARALLEL=1
INTERVAL=1
JSON=0
WINDOW=""
IPERF_EXTRA=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- iperf3-client-args...]

Start both winject-managers and run iperf3 over the TCP forward path.
iperf3's control+data sockets are multiplexed over the single manager
TCP session via tools/tcp_mux_tunnel.py.

Radio / host:
  --a IP            radio A Ethernet IP (default: $RADIO_A)
  --b IP            radio B Ethernet IP (default: $RADIO_B)
  --host IP         host IP radios send upstream_tx to (auto-detect if omitted)
  --no-cca          disable CCA on both radios before the test
  --cca             enable CCA (default)

Test selection:
  --dir DIR         ab | ba | both | bidir  (default: both)
                    ab/ba/both = sequential unidirectional runs
                    bidir = single iperf3 --bidir on the A→B path
  -t, --time SEC    test duration seconds (default: $TIME)
  -b, --bitrate R   target bitrate (e.g. 8M, 4000K); omit for unlimited TCP
  -R, --reverse     reverse (server sends; client receives)
  -P, --parallel N  parallel streams (default: 1)
  -i, --interval SEC reporting interval (default: $INTERVAL)
  -w, --window SIZE TCP window / socket buffer hint for iperf3
  --json            ask iperf3 for JSON output

Examples:
  $(basename "$0") --dir ab --time 15
  $(basename "$0") --dir both --bitrate 6M --no-cca
  $(basename "$0") --dir bidir --time 20
  $(basename "$0") -- --get-server-output
EOF
}

if [[ "${1:-}" == "--" ]]; then
  shift
fi

# Legacy positional IPs: RADIO_A RADIO_B [HOST_IP] [args...]
if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  RADIO_A="$1"
  shift
  if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    RADIO_B="$1"
    shift
  fi
  if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    HOST_IP="$1"
    HOST_SET=1
    shift
  fi
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      IPERF_EXTRA+=("$@")
      break
      ;;
    --a)
      RADIO_A="${2:?--a needs an IP}"
      shift 2
      ;;
    --a=*)
      RADIO_A="${1#--a=}"
      shift
      ;;
    --b)
      RADIO_B="${2:?--b needs an IP}"
      shift 2
      ;;
    --b=*)
      RADIO_B="${1#--b=}"
      shift
      ;;
    --host)
      HOST_IP="${2:?--host needs an IP}"
      HOST_SET=1
      shift 2
      ;;
    --host=*)
      HOST_IP="${1#--host=}"
      HOST_SET=1
      shift
      ;;
    --no-cca)
      PREP_EXTRA+=(--no-cca)
      shift
      ;;
    --cca)
      PREP_EXTRA+=(--cca)
      shift
      ;;
    --dir)
      DIR="${2:?--dir needs ab|ba|both|bidir}"
      shift 2
      ;;
    --dir=*)
      DIR="${1#--dir=}"
      shift
      ;;
    -t|--time)
      TIME="${2:?--time needs seconds}"
      shift 2
      ;;
    --time=*)
      TIME="${1#--time=}"
      shift
      ;;
    -b|--bitrate)
      BITRATE="${2:?--bitrate needs a rate}"
      shift 2
      ;;
    --bitrate=*)
      BITRATE="${1#--bitrate=}"
      shift
      ;;
    -u|--udp)
      echo "error: iperf3 UDP is not supported — manager path is TCP_SERVER/CLIENT_FORWARDING" >&2
      exit 1
      ;;
    -R|--reverse)
      REVERSE=1
      shift
      ;;
    -P|--parallel)
      PARALLEL="${2:?--parallel needs a count}"
      shift 2
      ;;
    --parallel=*)
      PARALLEL="${1#--parallel=}"
      shift
      ;;
    -i|--interval)
      INTERVAL="${2:?--interval needs seconds}"
      shift 2
      ;;
    --interval=*)
      INTERVAL="${1#--interval=}"
      shift
      ;;
    -w|--window)
      WINDOW="${2:?--window needs a size}"
      shift 2
      ;;
    --window=*)
      WINDOW="${1#--window=}"
      shift
      ;;
    --json)
      JSON=1
      shift
      ;;
    *)
      IPERF_EXTRA+=("$1")
      shift
      ;;
  esac
done

case "$DIR" in
  ab|ba|both|bidir) ;;
  *)
    echo "error: --dir must be ab, ba, both, or bidir (got: $DIR)" >&2
    exit 1
    ;;
esac

if ! command -v iperf3 >/dev/null 2>&1; then
  echo "error: iperf3 not found on PATH" >&2
  exit 1
fi
if [[ ! -f "$MUX_TOOL" ]]; then
  echo "error: missing $MUX_TOOL" >&2
  exit 1
fi

if [[ "$HOST_SET" -eq 0 ]]; then
  HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 2323)); print(s.getsockname()[0]); s.close()")"
fi

ensure_winject_manager "$ROOT"

echo "configuring radios (fixed forward ports 9210/9220)..."
python3 "$ROOT/scripts/prepare_radios_for_manager.py" \
  --a "$RADIO_A" --b "$RADIO_B" --host "$HOST_IP" --verbose \
  "${PREP_EXTRA[@]+"${PREP_EXTRA[@]}"}" || exit 1

patch_conf() {
  local file="$1" device="$2"
  sed -e "s/^winject\.device.*/winject.device        = ${device}/" \
      -e "s/^winject\.local_ip.*/winject.local_ip      = ${HOST_IP}/" \
      "$file"
}

CONF_A_RUN="$LOG_DIR/winject_a.conf"
CONF_B_RUN="$LOG_DIR/winject_b.conf"
patch_conf "$CONF_A" "$RADIO_A" >"$CONF_A_RUN"
patch_conf "$CONF_B" "$RADIO_B" >"$CONF_B_RUN"

PID_A=""
PID_B=""
PID_IPERF=""
PID_DEMUX=""
PID_MUX=""

stop_path() {
  for pid in "${PID_MUX:-}" "${PID_IPERF:-}" "${PID_DEMUX:-}"; do
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; fi
  done
  PID_MUX=""
  PID_IPERF=""
  PID_DEMUX=""
  sleep 0.2
}

cleanup() {
  stop_path
  if [[ -n "${PID_A:-}" ]]; then kill "$PID_A" 2>/dev/null || true; fi
  if [[ -n "${PID_B:-}" ]]; then kill "$PID_B" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

pkill -f "winject-manager.*winject" 2>/dev/null || true
for p in "$PORT_RECV_AB" "$PORT_RECV_BA" "$MUX_AB" "$MUX_BA" "$IPERF_AB" "$IPERF_BA"; do
  fuser -k "${p}/tcp" 2>/dev/null || true
done
sleep 1

echo "managers: A=$RADIO_A B=$RADIO_B host=$HOST_IP logs=$LOG_DIR"
"$MANAGER" "$CONF_A_RUN" >"$LOG_DIR/manager_a.log" 2>&1 &
PID_A=$!
"$MANAGER" "$CONF_B_RUN" >"$LOG_DIR/manager_b.log" 2>&1 &
PID_B=$!

echo "waiting for managers..."
for i in $(seq 1 20); do
  if grep -q "manager running" "$LOG_DIR/manager_a.log" \
    && grep -q "manager running" "$LOG_DIR/manager_b.log"; then
    break
  fi
  sleep 0.25
done
if ! grep -q "manager running" "$LOG_DIR/manager_a.log"; then
  echo "manager A upstream setup failed; tail $LOG_DIR/manager_a.log"
  tail -20 "$LOG_DIR/manager_a.log"
  exit 1
fi
if ! grep -q "manager running" "$LOG_DIR/manager_b.log"; then
  echo "manager B upstream setup failed; tail $LOG_DIR/manager_b.log"
  tail -20 "$LOG_DIR/manager_b.log"
  exit 1
fi

wait_listen() {
  local port="$1" deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    if ss -ltn 2>/dev/null | grep -qE ":${port}\\s"; then
      return 0
    fi
    sleep 0.1
  done
  echo "error: nothing listening on 127.0.0.1:$port" >&2
  return 1
}

build_client_args() {
  local -a args=(-t "$TIME" -i "$INTERVAL" -P "$PARALLEL")
  if [[ -n "$BITRATE" ]]; then
    args+=(-b "$BITRATE")
  fi
  if [[ "$REVERSE" -eq 1 ]]; then
    args+=(-R)
  fi
  if [[ -n "$WINDOW" ]]; then
    args+=(-w "$WINDOW")
  fi
  if [[ "$JSON" -eq 1 ]]; then
    args+=(--json)
  fi
  if [[ ${#IPERF_EXTRA[@]} -gt 0 ]]; then
    args+=("${IPERF_EXTRA[@]}")
  fi
  printf '%s\n' "${args[@]}"
}

# Start demux on manager recv port, iperf3 server on local port, mux on local port.
start_path() {
  local tag="$1" demux_port="$2" iperf_port="$3" mux_port="$4" tunnel_port="$5"
  stop_path

  python3 "$MUX_TOOL" --demux \
    --listen "127.0.0.1:${demux_port}" \
    --target "127.0.0.1:${iperf_port}" \
    >"$LOG_DIR/demux_${tag}.log" 2>&1 &
  PID_DEMUX=$!
  wait_listen "$demux_port"

  local -a srv=(iperf3 -s -B 127.0.0.1 -p "$iperf_port" --one-off)
  if [[ "$JSON" -eq 1 ]]; then
    srv+=(--json)
  fi
  "${srv[@]}" >"$LOG_DIR/iperf_srv_${tag}.log" 2>&1 &
  PID_IPERF=$!
  wait_listen "$iperf_port"

  python3 "$MUX_TOOL" --mux \
    --listen "127.0.0.1:${mux_port}" \
    --tunnel "127.0.0.1:${tunnel_port}" \
    >"$LOG_DIR/mux_${tag}.log" 2>&1 &
  PID_MUX=$!
  wait_listen "$mux_port"
}

run_client() {
  local label="$1" mux_port="$2" tag="$3"
  shift 3
  local -a client_args=("$@")
  echo
  echo "========== iperf3 $label =========="
  echo "client → mux :$mux_port → manager tunnel → demux → iperf3 -s"
  set +e
  iperf3 -c 127.0.0.1 -p "$mux_port" "${client_args[@]}" \
    | tee "$LOG_DIR/iperf_cli_${tag}.log"
  local rc=${PIPESTATUS[0]}
  set -e
  if [[ "$rc" -ne 0 ]]; then
    echo "iperf3 client failed (rc=$rc)"
    echo "--- server ---"
    tail -30 "$LOG_DIR/iperf_srv_${tag}.log" || true
    echo "--- mux ---"
    tail -30 "$LOG_DIR/mux_${tag}.log" || true
    echo "--- demux ---"
    tail -30 "$LOG_DIR/demux_${tag}.log" || true
  fi
  return "$rc"
}

FAIL=0
mapfile -t CLIENT_ARGS < <(build_client_args)

if [[ "$DIR" == "ab" || "$DIR" == "both" ]]; then
  start_path ab "$PORT_RECV_AB" "$IPERF_AB" "$MUX_AB" "$PORT_SEND_AB"
  if ! run_client "A→B" "$MUX_AB" ab "${CLIENT_ARGS[@]}"; then
    FAIL=1
  fi
fi

if [[ "$DIR" == "ba" || "$DIR" == "both" ]]; then
  start_path ba "$PORT_RECV_BA" "$IPERF_BA" "$MUX_BA" "$PORT_SEND_BA"
  if ! run_client "B→A" "$MUX_BA" ba "${CLIENT_ARGS[@]}"; then
    FAIL=1
  fi
fi

if [[ "$DIR" == "bidir" ]]; then
  start_path bidir "$PORT_RECV_AB" "$IPERF_AB" "$MUX_AB" "$PORT_SEND_AB"
  BIDIR_ARGS=("${CLIENT_ARGS[@]}" --bidir)
  if ! run_client "A↔B bidir" "$MUX_AB" bidir "${BIDIR_ARGS[@]}"; then
    FAIL=1
  fi
fi

stop_path

echo
echo "=== iperf3 done (logs $LOG_DIR) ==="
exit "$FAIL"
