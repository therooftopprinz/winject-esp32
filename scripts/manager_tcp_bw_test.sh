#!/usr/bin/env bash
# Start winject-manager on both radios and run bw_test over manager TCP forwarding.
#
# Edit configuration/winject-tests/bw_a.cfg / bw_b.cfg for your radio IPs and host local_ip,
# or pass RADIO_A RADIO_B HOST_IP as the first three arguments.
#
# Usage:
#   ./scripts/manager_tcp_bw_test.sh
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
BW_ARGS=()

if [[ "${1:-}" == "--" ]]; then
  BW_ARGS=("${@:2}")
elif [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  RADIO_A="$1"
  [[ $# -ge 2 ]] && RADIO_B="$2"
  [[ $# -ge 3 ]] && HOST_IP="$3"
  [[ $# -ge 4 ]] && BW_ARGS=("${@:4}")
elif [[ $# -ge 1 ]]; then
  BW_ARGS=("$@")
fi

# Help passthrough: avoid touching radios/managers for usage-only calls.
for a in "${BW_ARGS[@]+"${BW_ARGS[@]}"}"; do
  if [[ "$a" == "-h" || "$a" == "--help" ]]; then
    exec python3 "$ROOT/tools/bw_test.py" --help
  fi
done

ensure_winject_manager "$ROOT"

echo "configuring radios (fixed forward ports 9210/9220)..."
PREP_EXTRA=()
for a in "${BW_ARGS[@]+"${BW_ARGS[@]}"}"; do
  if [[ "$a" == "--no-cca" ]]; then
    PREP_EXTRA+=(--no-cca)
  fi
done
python3 "$ROOT/scripts/prepare_radios_for_manager.py" --a "$RADIO_A" --b "$RADIO_B" --host "$HOST_IP" --verbose "${PREP_EXTRA[@]}" || exit 1

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
exec python3 "$ROOT/tools/bw_test.py" \
  --tcp \
  --skip-config \
  --a "$RADIO_A" \
  --b "$RADIO_B" \
  --host "$HOST_IP" \
  "${BW_ARGS[@]}"
