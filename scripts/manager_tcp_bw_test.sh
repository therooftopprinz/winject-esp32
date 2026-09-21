#!/usr/bin/env bash
# Compatibility wrapper — prefer scripts/manager_bw_test.sh
# (UDP default; pass --tcp for the old TCP ARQ path).
exec "$(cd "$(dirname "$0")" && pwd)/manager_bw_test.sh" --tcp "$@"
