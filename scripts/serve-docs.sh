#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/scripts/.venv"
REQS="$ROOT/scripts/requirements-docs.txt"

if [[ ! -x "$VENV/bin/python" ]]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q -r "$REQS"
fi

exec "$VENV/bin/python" "$ROOT/scripts/docs_server.py" "$@"
