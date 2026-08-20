#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${1:-$ROOT/third_party/librga/rk3399/librga.so.2}"
COMMIT="fb3357d09008222bc5e27bdaadf74a0c5ea4c86e"
SHA256="3da1413445885420abf00821640ec8a37289ec176fe4ffeee0de5f68418ed50e"
ARCHIVE_SHA256="44ede48c0dd3f6401d5081e15cbb8c1eeecb6eb5760527c7878ebb5615967285"
URL="https://codeload.github.com/airockchip/librga/tar.gz/$COMMIT"
INCLUDE_OUTPUT="${REMYDESK_RK3399_RGA_INCLUDE_DIR:-$(dirname "$OUTPUT")/include/rga}"
TMP="$(mktemp)"
EXTRACT_DIR="$(mktemp -d)"

cleanup() {
  rm -f "$TMP"
  rm -rf "$EXTRACT_DIR"
}
trap cleanup EXIT

download() {
  if command -v curl >/dev/null 2>&1; then
    curl -fL --connect-timeout 15 -o "$TMP" "$URL"
  elif command -v wget >/dev/null 2>&1; then
    wget --timeout=15 -O "$TMP" "$URL"
  else
    return 127
  fi
}

downloaded=0
for attempt in 1 2 3 4; do
  if download; then
    downloaded=1
    break
  fi
  echo "librga download attempt $attempt failed" >&2
  : >"$TMP"
  sleep "$attempt"
done
[[ "$downloaded" -eq 1 ]] || {
  echo "unable to download pinned Rockchip librga source archive" >&2
  exit 1
}

printf '%s  %s\n' "$ARCHIVE_SHA256" "$TMP" | sha256sum -c -
tar -xzf "$TMP" -C "$EXTRACT_DIR" --strip-components=1
RUNTIME="$EXTRACT_DIR/libs/Linux/gcc-aarch64/librga.so"
[[ -f "$RUNTIME" && -f "$EXTRACT_DIR/include/im2d.h" ]] || {
  echo "pinned Rockchip librga archive is incomplete" >&2
  exit 1
}
printf '%s  %s\n' "$SHA256" "$RUNTIME" | sha256sum -c -
mkdir -p "$(dirname "$OUTPUT")"
install -m 0755 "$RUNTIME" "$OUTPUT"
mkdir -p "$INCLUDE_OUTPUT"
cp -a "$EXTRACT_DIR/include/." "$INCLUDE_OUTPUT/"
install -m 0644 "$EXTRACT_DIR/COPYING" "$(dirname "$OUTPUT")/LICENSE"
echo "Installed pinned Rockchip librga 1.10.0 runtime and headers: $OUTPUT"
