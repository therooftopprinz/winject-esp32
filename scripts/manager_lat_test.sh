#!/usr/bin/env bash
# One-way air TX-RX latency through winject-manager on this host (ARM or x86).
# Both radios stay STANDALONE; both managers run locally.
#
# Cases (default all four):
#   raw      raw UDP (no FEC)
#   fec10-15 UDP RS_BLOCK_ERASURE k=10 n=15
#   fec10-11 UDP RS_BLOCK_ERASURE k=10 n=11
#   tcp      manager TCP ARQ (same ports as manager_tcp_bw_test.sh)
#
# Usage:
#   ./scripts/manager_lat_test.sh
#   ./scripts/manager_lat_test.sh --a 192.168.127.181 --b 192.168.128.119 --no-cca --count 800
#   CASES=raw,fec10-15 ./scripts/manager_lat_test.sh
#   ./scripts/manager_lat_test.sh 192.168.253.11 192.168.253.12 192.168.253.106

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"

CONF_UDP_A="$ROOT/configuration/winject-tests/lat_udp_a.cfg"
CONF_UDP_B="$ROOT/configuration/winject-tests/lat_udp_b.cfg"
CONF_TCP_A="$ROOT/configuration/winject-tests/bw_a.cfg"
CONF_TCP_B="$ROOT/configuration/winject-tests/bw_b.cfg"
LOG_DIR="${TMPDIR:-/tmp}/winject-lat-$$"
mkdir -p "$LOG_DIR"

RADIO_A="192.168.253.11"
RADIO_B="192.168.253.12"
HOST_IP="192.168.253.106"
HOST_SET=0
LAT_ARGS=()
CASES="${CASES:-raw,fec10-15,fec10-11,tcp}"

if [[ "${1:-}" == "--" ]]; then
  shift
fi

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

LAT_ARGS=("$@")

for a in "${LAT_ARGS[@]+"${LAT_ARGS[@]}"}"; do
  if [[ "$a" == "-h" || "$a" == "--help" ]]; then
    exec python3 "$ROOT/tools/lat_test.py" --help
  fi
done

# Strip runner-only flags from lat_test args.
PREP_EXTRA=()
PASS_ARGS=()
i=0
while [[ $i -lt ${#LAT_ARGS[@]} ]]; do
  a="${LAT_ARGS[$i]}"
  case "$a" in
    --a)
      i=$((i + 1))
      RADIO_A="${LAT_ARGS[$i]:-}"
      if [[ -z "$RADIO_A" ]]; then
        echo "--a needs an IP" >&2
        exit 1
      fi
      ;;
    --a=*)
      RADIO_A="${a#--a=}"
      ;;
    --b)
      i=$((i + 1))
      RADIO_B="${LAT_ARGS[$i]:-}"
      if [[ -z "$RADIO_B" ]]; then
        echo "--b needs an IP" >&2
        exit 1
      fi
      ;;
    --b=*)
      RADIO_B="${a#--b=}"
      ;;
    --host)
      i=$((i + 1))
      HOST_IP="${LAT_ARGS[$i]:-}"
      if [[ -z "$HOST_IP" ]]; then
        echo "--host needs an IP" >&2
        exit 1
      fi
      HOST_SET=1
      ;;
    --host=*)
      HOST_IP="${a#--host=}"
      HOST_SET=1
      ;;
    --no-cca)
      PREP_EXTRA+=(--no-cca)
      ;;
    --cca)
      PREP_EXTRA+=(--cca)
      ;;
    --cases)
      i=$((i + 1))
      CASES="${LAT_ARGS[$i]:-}"
      if [[ -z "$CASES" ]]; then
        echo "--cases needs a value (raw,fec10-15,fec10-11,tcp)" >&2
        exit 1
      fi
      ;;
    --cases=*)
      CASES="${a#--cases=}"
      ;;
    *)
      PASS_ARGS+=("$a")
      ;;
  esac
  i=$((i + 1))
done

if [[ "$HOST_SET" -eq 0 ]]; then
  HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 2323)); print(s.getsockname()[0]); s.close()")"
fi

ensure_winject_manager "$ROOT"

IFS=',' read -r -a CASE_LIST <<< "$CASES"
PID_A=""
PID_B=""

stop_managers() {
  local a="${PID_A}" b="${PID_B}"
  PID_A=""
  PID_B=""
  if [[ -n "$a" ]]; then kill "$a" 2>/dev/null || true; fi
  if [[ -n "$b" ]]; then kill "$b" 2>/dev/null || true; fi
  if [[ -n "$a" ]]; then wait "$a" 2>/dev/null || true; fi
  if [[ -n "$b" ]]; then wait "$b" 2>/dev/null || true; fi
}

cleanup() {
  stop_managers
}
trap cleanup EXIT INT TERM

patch_conf() {
  local src="$1" device="$2" dest="$3"
  sed -e "s/^winject\.device.*/winject.device        = ${device}/" \
      -e "s/^winject\.local_ip.*/winject.local_ip      = ${HOST_IP}/" \
      "$src" >"$dest"
}

append_fec() {
  local file="$1" k="$2" n="$3"
  local i
  for i in 0 1; do
    cat >>"$file" <<EOF
upstream-${i}.fec.type         = RS_BLOCK_ERASURE
upstream-${i}.fec.k            = ${k}
upstream-${i}.fec.n            = ${n}
EOF
  done
}

start_managers() {
  local conf_a="$1" conf_b="$2" tag="$3"
  stop_managers
  pkill -f "winject-manager.*winject" 2>/dev/null || true
  pkill -x winject-manager 2>/dev/null || true
  sleep 0.4
  echo "managers [$tag]: A=$RADIO_A B=$RADIO_B host=$HOST_IP"
  "$MANAGER" "$conf_a" >"$LOG_DIR/manager_a_${tag}.log" 2>&1 &
  PID_A=$!
  "$MANAGER" "$conf_b" >"$LOG_DIR/manager_b_${tag}.log" 2>&1 &
  PID_B=$!
  local i
  for i in $(seq 1 40); do
    if grep -q "manager running" "$LOG_DIR/manager_a_${tag}.log" \
      && grep -q "manager running" "$LOG_DIR/manager_b_${tag}.log"; then
      return 0
    fi
    if ! kill -0 "$PID_A" 2>/dev/null || ! kill -0 "$PID_B" 2>/dev/null; then
      break
    fi
    sleep 0.15
  done
  echo "manager failed to start [$tag]"
  echo "--- A ---"
  tail -30 "$LOG_DIR/manager_a_${tag}.log" || true
  echo "--- B ---"
  tail -30 "$LOG_DIR/manager_b_${tag}.log" || true
  return 1
}

echo "configuring radios (fixed forward ports 9210/9220)..."
python3 "$ROOT/scripts/prepare_radios_for_manager.py" \
  --a "$RADIO_A" --b "$RADIO_B" --host "$HOST_IP" --verbose "${PREP_EXTRA[@]+"${PREP_EXTRA[@]}"}" \
  || exit 1

RESULTS="$LOG_DIR/results.txt"
: >"$RESULTS"
FAIL=0

run_lat() {
  local label="$1"
  shift
  echo
  echo "========== $label =========="
  local rc=0
  python3 "$ROOT/tools/lat_test.py" --label "$label" "${PASS_ARGS[@]+"${PASS_ARGS[@]}"}" "$@" \
    | tee -a "$RESULTS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    FAIL=1
  fi
  return 0
}

for case in "${CASE_LIST[@]}"; do
  case="$(echo "$case" | tr -d '[:space:]')"
  [[ -z "$case" ]] && continue
  CONF_A_RUN="$LOG_DIR/${case}_a.conf"
  CONF_B_RUN="$LOG_DIR/${case}_b.conf"
  case "$case" in
    raw|raw-udp|udp)
      patch_conf "$CONF_UDP_A" "$RADIO_A" "$CONF_A_RUN"
      patch_conf "$CONF_UDP_B" "$RADIO_B" "$CONF_B_RUN"
      start_managers "$CONF_A_RUN" "$CONF_B_RUN" raw
      run_lat raw
      ;;
    fec10-15|fec-10-15|fec15)
      patch_conf "$CONF_UDP_A" "$RADIO_A" "$CONF_A_RUN"
      patch_conf "$CONF_UDP_B" "$RADIO_B" "$CONF_B_RUN"
      append_fec "$CONF_A_RUN" 10 15
      append_fec "$CONF_B_RUN" 10 15
      start_managers "$CONF_A_RUN" "$CONF_B_RUN" fec10-15
      run_lat fec10-15
      ;;
    fec10-11|fec-10-11|fec11)
      patch_conf "$CONF_UDP_A" "$RADIO_A" "$CONF_A_RUN"
      patch_conf "$CONF_UDP_B" "$RADIO_B" "$CONF_B_RUN"
      append_fec "$CONF_A_RUN" 10 11
      append_fec "$CONF_B_RUN" 10 11
      start_managers "$CONF_A_RUN" "$CONF_B_RUN" fec10-11
      run_lat fec10-11
      ;;
    tcp)
      patch_conf "$CONF_TCP_A" "$RADIO_A" "$CONF_A_RUN"
      patch_conf "$CONF_TCP_B" "$RADIO_B" "$CONF_B_RUN"
      start_managers "$CONF_A_RUN" "$CONF_B_RUN" tcp
      run_lat tcp --tcp
      ;;
    *)
      echo "unknown case: $case (raw, fec10-15, fec10-11, tcp)" >&2
      FAIL=1
      ;;
  esac
done

stop_managers

echo
echo "=== latency summary (logs $LOG_DIR) ==="
if grep -h '^RESULT ' "$RESULTS" 2>/dev/null; then
  true
else
  echo "(no RESULT lines)"
  FAIL=1
fi

exit "$FAIL"
