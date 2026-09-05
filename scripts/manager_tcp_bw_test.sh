#!/usr/bin/env bash
# Start winject-manager on both radios and run bw_test over manager TCP forwarding.
#
# Edit configuration/winject-tests/bw_a.cfg / bw_b.cfg for your radio IPs and host local_ip,
# or pass RADIO_A RADIO_B HOST_IP as the first three arguments.
#
# Usage:
#   ./scripts/manager_tcp_bw_test.sh
#   ./scripts/manager_tcp_bw_test.sh --a 192.168.127.181 --b 192.168.128.119 --modulation OFDM_24M
#   ./scripts/manager_tcp_bw_test.sh 192.168.253.11 192.168.253.12 192.168.253.106
#   ./scripts/manager_tcp_bw_test.sh -- --modulation OFDM_24M --bidir

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"
CONF_A="$ROOT/configuration/winject-tests/bw_a.cfg"
CONF_B="$ROOT/configuration/winject-tests/bw_b.cfg"
LOG_DIR="${TMPDIR:-/tmp}/winject-manager-$$"
mkdir -p "$LOG_DIR"

RADIO_A="192.168.253.11"
RADIO_B="192.168.253.12"
HOST_IP="192.168.253.106"
HOST_SET=0
BW_ARGS=()
PREP_EXTRA=()

if [[ "${1:-}" == "--" ]]; then
  shift
fi

# Legacy positional IPs: RADIO_A RADIO_B [HOST_IP] [bw args...]
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
      exec python3 "$ROOT/tools/bw_test.py" --help
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
      BW_ARGS+=("$1")
      shift
      ;;
    *)
      BW_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ "$HOST_SET" -eq 0 ]]; then
  HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 2323)); print(s.getsockname()[0]); s.close()")"
fi

ensure_winject_manager "$ROOT"

echo "configuring radios (fixed forward ports 9210/9220)..."
python3 "$ROOT/scripts/prepare_radios_for_manager.py" --a "$RADIO_A" --b "$RADIO_B" --host "$HOST_IP" --verbose "${PREP_EXTRA[@]+"${PREP_EXTRA[@]}"}" || exit 1

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

cleanup() {
  if [[ -n "${PID_A:-}" ]]; then kill "$PID_A" 2>/dev/null || true
  fi
  if [[ -n "${PID_B:-}" ]]; then kill "$PID_B" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

pkill -f "winject-manager.*winject" 2>/dev/null || true
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

echo "running bw_test --tcp --a $RADIO_A --b $RADIO_B --host $HOST_IP ${BW_ARGS[*]}"
# Do not exec: the EXIT trap must run to kill managers. exec would replace this
# shell and leave winject-manager orphans retxing CONNECT forever.
set +e
python3 "$ROOT/tools/bw_test.py" \
  --tcp \
  --skip-config \
  --a "$RADIO_A" \
  --b "$RADIO_B" \
  --host "$HOST_IP" \
  "${BW_ARGS[@]}"
status=$?
set -e
exit "$status"
