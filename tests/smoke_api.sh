#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${REMYDESK_TEST_BINARY:-$ROOT/build/remydeskd}"
PORT="${REMYDESK_TEST_PORT:-18019}"
BASE="/tmp/remydesk-test-$$"
PID=""

cleanup() {
  [[ -n "$PID" ]] && kill "$PID" 2>/dev/null || true
  [[ -n "$PID" ]] && wait "$PID" 2>/dev/null || true
  rm -rf "$BASE"
}
trap cleanup EXIT

[[ -x "$SERVER" ]] || { echo "test server is missing: $SERVER" >&2; exit 1; }
mkdir -p "$BASE/data" "$BASE/state" "$BASE/run"

env \
  REMYDESK_PORT="$PORT" \
  REMYDESK_AUTO_HOTSPOT=false \
  REMYDESK_STORAGE_ROOT="$BASE/data" \
  REMYDESK_STATE_DIR="$BASE/state" \
  REMYDESK_RUNTIME_DIR="$BASE/run" \
  REMYDESK_HARDWARE_FILE="$BASE/state/hardware.json" \
  REMYDESK_WEB_ROOT="$ROOT/web" \
  "$SERVER" >"$BASE.log" 2>&1 &
PID="$!"

READY=0
for _ in $(seq 1 30); do
  if curl -fsS "http://127.0.0.1:$PORT/api/health" >/dev/null; then
    READY=1
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if [[ "$READY" -ne 1 ]]; then
  echo "RemyDesk test server did not become ready" >&2
  cat "$BASE.log" >&2 || true
  exit 1
fi

curl -fsS "http://127.0.0.1:$PORT/api/health" | grep -q '"ok":true'
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"path":"","name":"smoke"}' "http://127.0.0.1:$PORT/api/mkdir" >/dev/null
printf 'streaming upload\n' | curl -fsS -X POST --data-binary @- \
  "http://127.0.0.1:$PORT/api/upload?path=smoke&name=test.txt" >/dev/null
curl -fsS "http://127.0.0.1:$PORT/file?path=smoke/test.txt" | grep -q 'streaming upload'
printf 'move me\n' | curl -fsS -X POST --data-binary @- \
  "http://127.0.0.1:$PORT/api/upload?path=&name=root.txt" >/dev/null
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"source":"root.txt","destination":"smoke"}' "http://127.0.0.1:$PORT/api/move" >/dev/null
curl -fsS "http://127.0.0.1:$PORT/file?path=smoke/root.txt" | grep -q 'move me'
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"layout":{"smoke":{"rx":0.25,"ry":0.75}}}' "http://127.0.0.1:$PORT/api/desktop/layout" >/dev/null
curl -fsS "http://127.0.0.1:$PORT/api/desktop/layout" | grep -q '"rx":0.25'
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"text":"C++ API smoke"}' "http://127.0.0.1:$PORT/api/note" >/dev/null
curl -fsS "http://127.0.0.1:$PORT/api/note" | grep -q 'C++ API smoke'

code="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/files?path=../etc")"
[[ "$code" == "400" ]]

echo "RemyDesk API smoke test passed"
