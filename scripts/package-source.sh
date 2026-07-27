#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT="$(dirname "$ROOT")"
NAME="$(basename "$ROOT")"
VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION" 2>/dev/null || date +%Y%m%d_%H%M%S)"
OUTPUT="${1:-$PARENT/RemyDesk-$VERSION-source.tar.gz}"

tar -C "$PARENT" \
  --exclude="$NAME/build" \
  --exclude="$NAME/build-*" \
  --exclude="$NAME/dist" \
  --exclude="$NAME/.git" \
  --exclude="$NAME/*.before-*" \
  --exclude="$NAME/scripts/*.before-*" \
  --exclude="$NAME/native/desktop-streamer/drm_fb_probe" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_stream" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_snapshot" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_frame" \
  --exclude="$NAME/native/webrtc-publisher/remydesk-webrtc-publisher" \
  -czf "$OUTPUT" "$NAME"

sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "Source bundle created:"
echo "  $OUTPUT"
echo "  $OUTPUT.sha256"
