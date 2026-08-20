#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(nproc)}"
BUILD_PROFILE="${REMYDESK_BUILD_PROFILE:-auto}"

if [[ "$BUILD_PROFILE" == auto ]]; then
  BUILD_PROFILE="$($ROOT/scripts/detect-profile.sh)"
fi

go_version_ok() {
  local candidate="$1"
  local version major minor rest
  [[ -x "$candidate" ]] || return 1
  version="$("$candidate" env GOVERSION 2>/dev/null)" || return 1
  version="${version#go}"
  major="${version%%.*}"
  rest="${version#*.}"
  minor="${rest%%.*}"
  [[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] || return 1
  (( major > 1 || (major == 1 && minor >= 22) ))
}

select_go() {
  local candidate build_user build_home
  local candidates=()
  [[ -n "${GO:-}" ]] && candidates+=("$GO")

  build_user="${SUDO_USER:-$(id -un)}"
  build_home="$(getent passwd "$build_user" 2>/dev/null | cut -d: -f6)"
  shopt -s nullglob
  if [[ -n "$build_home" ]]; then
    candidates+=("$build_home"/.local/toolchains/go*/bin/go)
  fi
  candidates+=(/usr/local/go/bin/go)
  if command -v go >/dev/null 2>&1; then
    candidates+=("$(command -v go)")
  fi
  shopt -u nullglob

  for candidate in "${candidates[@]}"; do
    if go_version_ok "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
cmake --build "$ROOT/build" -j"$JOBS"

NATIVE_MAKE_ARGS=()
if [[ "$BUILD_PROFILE" == "firefly-rk3399" ]]; then
  RK3399_RGA_ROOT="$ROOT/third_party/librga/rk3399"
  if [[ ! -f "$RK3399_RGA_ROOT/librga.so.2" ||
        ! -f "$RK3399_RGA_ROOT/include/rga/im2d.h" ]]; then
    "$ROOT/scripts/fetch-rk3399-librga.sh" "$RK3399_RGA_ROOT/librga.so.2"
  fi
  NATIVE_MAKE_ARGS+=("RK3399_RGA_ROOT=$RK3399_RGA_ROOT")
fi
make -C "$ROOT/native/desktop-streamer" -j"$JOBS" \
  "${NATIVE_MAKE_ARGS[@]}" drm_fb_probe drm_rga_mpp_stream

if [[ "${REMYDESK_USE_BUNDLED_PUBLISHER:-0}" == "1" ]]; then
  PUBLISHER="$ROOT/native/webrtc-publisher/remydesk-webrtc-publisher"
  [[ -x "$PUBLISHER" ]] || {
    echo "The bundled ARM64 WebRTC publisher is missing or not executable." >&2
    exit 1
  }
  echo "Using bundled ARM64 WebRTC publisher."
else
  GO_BIN="$(select_go)" || {
    echo "Go 1.22 or newer is required. Set GO=/path/to/go, install a current toolchain," >&2
    echo "or install from the single-file RK3588 bundle, which includes the publisher." >&2
    exit 1
  }
  echo "Using $("$GO_BIN" version)"
  (
    cd "$ROOT/native/webrtc-publisher"
    "$GO_BIN" test ./...
    VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION")"
    CGO_ENABLED="${CGO_ENABLED:-0}" "$GO_BIN" build -trimpath \
      -ldflags "-s -w -X main.version=$VERSION" -o remydesk-webrtc-publisher .
  )
fi

echo "RemyDesk build complete:"
echo "  $ROOT/build/remydeskd"
echo "  $ROOT/native/desktop-streamer/drm_rga_mpp_stream"
echo "  $ROOT/native/webrtc-publisher/remydesk-webrtc-publisher"
