#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION")"
DIST="${1:-$ROOT/dist}"

[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$ ]] || {
  echo "Invalid VERSION: $VERSION" >&2
  exit 1
}

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/remydesk-release.XXXXXX")"
cleanup() {
  rm -rf "$STAGE"
}
trap cleanup EXIT

REMYDESK_VERSION="$VERSION" \
  "$ROOT/scripts/make-single-file-installer.sh" \
  "$STAGE/RemyDesk-RK3588-$VERSION-installer.sh"
cp "$STAGE/RemyDesk-RK3588-$VERSION-installer.sh" "$STAGE/RemyDesk-RK3588-installer.sh"

if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
   git -C "$ROOT" diff --quiet && git -C "$ROOT" diff --cached --quiet; then
  git -C "$ROOT" archive --format=tar.gz --prefix="RemyDesk-$VERSION/" \
    -o "$STAGE/RemyDesk-$VERSION-source.tar.gz" HEAD
else
  NAME="$(basename "$ROOT")"
  tar -C "$(dirname "$ROOT")" -czf "$STAGE/RemyDesk-$VERSION-source.tar.gz" \
    --exclude="$NAME/.git" \
    --exclude="$NAME/build*" \
    --exclude="$NAME/dist" \
    --exclude="$NAME/*.before-*" \
    --exclude="$NAME/vendor-test" \
    --exclude="$NAME/native/desktop-streamer/drm_fb_probe" \
    --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_stream" \
    --exclude="$NAME/native/desktop-streamer/drm_rga_snapshot" \
    --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_frame" \
    --exclude="$NAME/native/webrtc-publisher/remydesk-webrtc-publisher" \
    "$NAME"
fi

(
  cd "$STAGE"
  sha256sum \
    "RemyDesk-RK3588-$VERSION-installer.sh" \
    RemyDesk-RK3588-installer.sh \
    "RemyDesk-$VERSION-source.tar.gz" >SHA256SUMS
)

rm -rf "$DIST"
mv "$STAGE" "$DIST"
trap - EXIT

printf 'Release assets created in %s\n' "$DIST"
ls -lh "$DIST"
