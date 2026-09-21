#!/usr/bin/env bash
# On-device wifi_tx bench (no Ethernet UDP inject) + USB WiFi monitor sniff.
#
# Radio builds MPDUs in wifi_bench and enqueues straight into wifi_tx.
# Host sniffs with scripts/usb_wifi_logger.py on a monitor iface.
#
# Usage:
#   ./scripts/wifi_bench_usb_test.sh
#   ./scripts/wifi_bench_usb_test.sh --a 192.168.253.9 --modulation OFDM_24M
#   ./scripts/wifi_bench_usb_test.sh --iface wlx3c789537952a --count 200 --kbps 2000

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

RADIO="192.168.253.9"
IFACE=""
CHANNEL=""
MODULATION="OFDM_24M"
DOMAIN="1234"
SIZE=64
COUNT=100
KBPS=2000
CCA=0
DURATION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --a) RADIO="${2:?}"; shift 2 ;;
    --a=*) RADIO="${1#--a=}"; shift ;;
    --iface) IFACE="${2:?}"; shift 2 ;;
    --iface=*) IFACE="${1#--iface=}"; shift ;;
    --channel) CHANNEL="${2:?}"; shift 2 ;;
    --channel=*) CHANNEL="${1#--channel=}"; shift ;;
    --modulation) MODULATION="${2:?}"; shift 2 ;;
    --modulation=*) MODULATION="${1#--modulation=}"; shift ;;
    --domain) DOMAIN="${2:?}"; shift 2 ;;
    --size) SIZE="${2:?}"; shift 2 ;;
    --count) COUNT="${2:?}"; shift 2 ;;
    --kbps) KBPS="${2:?}"; shift 2 ;;
    --duration) DURATION="${2:?}"; shift 2 ;;
    --cca) CCA=1; shift ;;
    --no-cca) CCA=0; shift ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

# Sniff through paced bench + console stalls (UDP can lag >15s under load).
if [[ -z "$DURATION" ]]; then
  DURATION="$(python3 - "$SIZE" "$COUNT" "$KBPS" <<'PY'
import math, sys
size, count, kbps = (int(x) for x in sys.argv[1:4])
est = (count * size * 8) / (kbps * 1000.0)
print(max(20, int(math.ceil(est)) + 25))
PY
)"
fi

console() {
  python3 - "$RADIO" "$@" <<'PY'
import socket, sys
ip = sys.argv[1]
cmds = sys.argv[2:]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
for c in cmds:
    s.sendto((c + "\n").encode(), (ip, 22))
    try:
        d, _ = s.recvfrom(8192)
        text = d.decode(errors="replace").strip()
        print(f"[{ip}] {c}")
        print(text[:300])
        if not text.lower().startswith("ok"):
            raise SystemExit(f"console nok: {c}")
    except socket.timeout:
        raise SystemExit(f"console timeout: {c}")
s.close()
PY
}

# Discover monitor iface if needed.
if [[ -z "$IFACE" ]]; then
  IFACE="$(python3 - <<'PY'
import subprocess
out = subprocess.check_output(["iw", "dev"], text=True)
cur = None
mon = []
for line in out.splitlines():
    line=line.strip()
    if line.startswith("Interface "):
        cur = line.split()[1]
    elif line.startswith("type ") and cur:
        if line.split()[1] == "monitor":
            mon.append(cur)
        cur = None
print(mon[0] if mon else "")
PY
)"
  if [[ -z "$IFACE" ]]; then
    echo "no monitor-mode WiFi iface found" >&2
    exit 1
  fi
fi

# Read radio channel if not given.
if [[ -z "$CHANNEL" ]]; then
  CHANNEL="$(python3 - <<PY
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2)
s.sendto(b"status\\n", ("$RADIO", 22))
text=s.recvfrom(4096)[0].decode(errors="replace")
s.close()
for part in text.replace("\\n"," ").split():
    if part.startswith("channel=") and part.split("=",1)[1].isdigit():
        print(part.split("=",1)[1]); break
else:
    print("11")
PY
)"
fi

SNIFF_LOG="${TMPDIR:-/tmp}/wifi_bench_sniff_$$.log"
SNIFF_PID=""
cleanup() {
  if [[ -n "${SNIFF_PID}" ]]; then
    kill "$SNIFF_PID" 2>/dev/null || true
    wait "$SNIFF_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "radio=$RADIO  iface=$IFACE  channel=$CHANNEL  mod=$MODULATION  domain=$DOMAIN"
echo "wifi_bench size=$SIZE count=$COUNT kbps=$KBPS  (on-device wifi_tx, no Ethernet inject)"

console \
  "wifi_bench_stop" \
  "set_mode STANDALONE" \
  "set_domain $DOMAIN" \
  "set_inject_sink wifi" \
  "unset_upstream_tx" \
  "unset_upstream_rx" \
  "set_channel $CHANNEL" \
  "set_modulation $MODULATION" \
  "set_cca_enabled $CCA"

INJECT_OK_BEFORE="$(python3 - "$RADIO" <<'PY'
import re, socket, sys
ip = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5.0)
s.sendto(b"status\n", (ip, 22))
text = s.recvfrom(65535)[0].decode(errors="replace")
m = re.search(r"inject_ok=(\d+)", text)
print(m.group(1) if m else "0")
s.close()
PY
)"

echo "starting USB sniff -> $SNIFF_LOG (duration=${DURATION}s)"
sudo -n python3 "$ROOT/scripts/usb_wifi_logger.py" \
  --iface "$IFACE" \
  --channel "$CHANNEL" \
  --bssid standalone \
  --duration "$DURATION" \
  --interval 2 \
  --quiet \
  >"$SNIFF_LOG" 2>&1 &
SNIFF_PID=$!
sleep 0.3

echo "wifi_bench_tx..."
# Reply can lag for seconds while wifi_bench + wifi_tx load the radio.
python3 - "$RADIO" "$SIZE" "$COUNT" "$KBPS" <<'PY'
import socket, sys
ip, size, count, kbps = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
cmd = f"wifi_bench_tx size={size} count={count} kbps={kbps}"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3.0)
s.sendto((cmd + "\n").encode(), (ip, 22))
try:
    text = s.recvfrom(8192)[0].decode(errors="replace").strip()
    print(f"[{ip}] {cmd}")
    print(text[:300])
except socket.timeout:
    print(f"[{ip}] {cmd}")
    print("(no reply within 3s — bench likely started; polling status)")
s.close()
PY

# Poll until idle; console UDP can be starved >2s during heavy inject.
BENCH_POLL_LOG="${TMPDIR:-/tmp}/wifi_bench_poll_$$.log"
python3 - "$RADIO" "$SIZE" "$COUNT" "$KBPS" <<'PY' | tee "$BENCH_POLL_LOG"
import re, socket, sys, time

ip = sys.argv[1]
size, count, kbps = (int(x) for x in sys.argv[2:5])
est_s = (count * size * 8) / (kbps * 1000.0) + 2.0
deadline = time.monotonic() + est_s + 45.0
last = ""
while time.monotonic() < deadline:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(15.0)
    s.sendto(b"wifi_bench_status\n", (ip, 22))
    try:
        last = s.recvfrom(4096)[0].decode(errors="replace").strip()
        print(last)
        if "running=0" in last:
            m = re.search(r"enq_ok=(\d+)", last)
            if m and int(m.group(1)) > 0:
                break
            if m and int(m.group(1)) == 0:
                # idle before start — keep polling
                pass
            else:
                break
    except socket.timeout:
        print("(wifi_bench_status timeout — retrying)")
    finally:
        s.close()
    time.sleep(0.5)
else:
    raise SystemExit("wifi_bench_status: bench did not finish before deadline")

m_ok = re.search(r"enq_ok=(\d+)", last)
m_fail = re.search(r"enq_fail=(\d+)", last)
m_el = re.search(r"elapsed_us=(-?\d+)", last)
print(
    "BENCH_SUMMARY "
    f"enq_ok={m_ok.group(1) if m_ok else 0} "
    f"enq_fail={m_fail.group(1) if m_fail else 0} "
    f"elapsed_us={m_el.group(1) if m_el else 0}",
    flush=True,
)
PY
ENQ_OK="$(awk '/^BENCH_SUMMARY / { for (i=1;i<=NF;i++) if ($i ~ /^enq_ok=/) { split($i,a,"="); print a[2]; exit } }' "$BENCH_POLL_LOG")"
ENQ_OK="${ENQ_OK:-0}"

python3 - "$RADIO" <<'PY' | head -40 || true
import socket, sys
ip = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(15.0)
s.sendto(b"status\n", (ip, 22))
print(s.recvfrom(65535)[0].decode(errors="replace"))
s.close()
PY

echo "waiting for sniffer..."
wait "$SNIFF_PID" || true
SNIFF_PID=""
echo "==== sniff ===="
cat "$SNIFF_LOG"

INJECT_OK_AFTER="$(python3 - "$RADIO" <<'PY'
import re, socket, sys
ip = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(15.0)
s.sendto(b"status\n", (ip, 22))
text = s.recvfrom(65535)[0].decode(errors="replace")
m = re.search(r"inject_ok=(\d+)", text)
print(m.group(1) if m else "0")
s.close()
PY
)"
INJECT_DELTA=$((INJECT_OK_AFTER - INJECT_OK_BEFORE))
echo "radio inject_ok delta=$INJECT_DELTA  wifi_bench enq_ok=$ENQ_OK"

# USB pass: sniffer matched WInject Addr3 (CA:FE:BA:BE:*) frames.
matched="$(grep '=== done ===' "$SNIFF_LOG" | grep -oE ' bssid=[0-9]+' | tail -1 | cut -d= -f2 || true)"
matched="${matched:-0}"
if [[ "$matched" -gt 0 ]]; then
  echo "PASS: USB sniffer saw $matched WInject frames (wifi_tx on air @ $MODULATION)"
  exit 0
fi

# On-device wifi_tx path succeeded even when USB dongle misses raw inject (common on this bench).
if [[ "$ENQ_OK" -gt 0 && "$INJECT_DELTA" -gt 0 ]]; then
  echo "PASS: radio wifi_bench enq_ok=$ENQ_OK inject_ok+=$INJECT_DELTA (USB sniffer bssid=0 — use peer ESP or air_bench_matrix.py for RX)"
  exit 0
fi

echo "FAIL: wifi_bench enq_ok=$ENQ_OK inject_ok+=$INJECT_DELTA; USB bssid=$matched" >&2
exit 1
