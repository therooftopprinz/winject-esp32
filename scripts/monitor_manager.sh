#!/usr/bin/env bash
# Tail winject-manager logs and show periodic stats / errors.
#
# Usage:
#   ./scripts/monitor_manager.sh                    # latest /tmp log dir
#   ./scripts/monitor_manager.sh /tmp/winject-manager-12345/manager_a.log
#   ./scripts/monitor_manager.sh --all              # both A and B logs

set -euo pipefail

if [[ "${1:-}" == "--all" ]]; then
  LOG_A="$(ls -td /tmp/winject-manager-*/manager_a.log 2>/dev/null | head -1)"
  LOG_B="$(ls -td /tmp/winject-manager-*/manager_b.log 2>/dev/null | head -1)"
  if [[ -z "$LOG_A" || -z "$LOG_B" ]]; then
    echo "no /tmp/winject-manager-* logs found; start manager_tcp_bw_test.sh first" >&2
    exit 1
  fi
  echo "watching A=$LOG_A  B=$LOG_B"
  exec tail -F "$LOG_A" "$LOG_B" | rg --line-buffered 'stats |ERR |WRN '
fi

LOG="${1:-}"
if [[ -z "$LOG" ]]; then
  LOG="$(ls -td /tmp/winject-manager-*/manager_a.log 2>/dev/null | head -1 || true)"
fi
if [[ -z "$LOG" || ! -f "$LOG" ]]; then
  echo "usage: $0 [manager_a.log]  or  $0 --all" >&2
  echo "no log found under /tmp/winject-manager-*" >&2
  exit 1
fi

echo "watching $LOG (stats + ERR/WRN)"
exec tail -F "$LOG" | rg --line-buffered 'stats |ERR |WRN '
