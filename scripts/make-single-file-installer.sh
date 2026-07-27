#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT="$(dirname "$ROOT")"
NAME="$(basename "$ROOT")"
VERSION="${REMYDESK_VERSION:-$(tr -d '[:space:]' <"$ROOT/VERSION" 2>/dev/null || date +%Y%m%d)}"
OUTPUT="${1:-$PARENT/RemyDesk-RK3588-$VERSION-installer.sh}"
GO_BIN="${GO:-}"
STAGE="$(mktemp -d)"
PAYLOAD="$STAGE/remydesk-payload.tar.gz"

cleanup() {
  rm -rf "$STAGE"
}
trap cleanup EXIT

find_go() {
  local candidate build_home version major minor rest
  local candidates=()
  [[ -n "$GO_BIN" ]] && candidates+=("$GO_BIN")
  build_home="${HOME:-}"
  shopt -s nullglob
  [[ -n "$build_home" ]] && candidates+=("$build_home"/.local/toolchains/go*/bin/go)
  candidates+=(/usr/local/go/bin/go)
  command -v go >/dev/null 2>&1 && candidates+=("$(command -v go)")
  shopt -u nullglob
  for candidate in "${candidates[@]}"; do
    [[ -x "$candidate" ]] || continue
    version="$("$candidate" env GOVERSION 2>/dev/null || true)"
    version="${version#go}"
    major="${version%%.*}"
    rest="${version#*.}"
    minor="${rest%%.*}"
    if [[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] &&
       (( major > 1 || (major == 1 && minor >= 22) )); then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

GO_BIN="$(find_go)" || {
  echo "Go 1.22 or newer is required once to generate the bundled publisher." >&2
  exit 1
}

mkdir -p "$STAGE/$NAME/native/webrtc-publisher"
tar -C "$PARENT" \
  --exclude="$NAME/.git" \
  --exclude="$NAME/build" \
  --exclude="$NAME/build-*" \
  --exclude="$NAME/dist" \
  --exclude="$NAME/native/desktop-streamer/drm_fb_probe" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_stream" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_snapshot" \
  --exclude="$NAME/native/desktop-streamer/drm_rga_mpp_frame" \
  --exclude="$NAME/native/webrtc-publisher/remydesk-webrtc-publisher" \
  --exclude="$NAME/RemyDesk-*-installer-*.sh" \
  --exclude="$NAME/scripts/*.before-*" \
  -cf - "$NAME" | tar -C "$STAGE" -xf -

(
  cd "$ROOT/native/webrtc-publisher"
  CGO_ENABLED=0 "$GO_BIN" test ./...
  CGO_ENABLED=0 GOOS=linux GOARCH=arm64 "$GO_BIN" build -trimpath -ldflags "-s -w" \
    -o "$STAGE/$NAME/native/webrtc-publisher/remydesk-webrtc-publisher" .
)
chmod 0755 "$STAGE/$NAME/native/webrtc-publisher/remydesk-webrtc-publisher"
tar -C "$STAGE" -czf "$PAYLOAD" "$NAME"
PAYLOAD_SHA256="$(sha256sum "$PAYLOAD" | awk '{print $1}')"
PAYLOAD_SIZE="$(stat -c '%s' "$PAYLOAD")"

cat >"$OUTPUT" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail

INSTALLER_VERSION="$VERSION"
PAYLOAD_SHA256="$PAYLOAD_SHA256"
PAYLOAD_SIZE="$PAYLOAD_SIZE"
SKIP_DEPS=0
VERIFY_ONLY=0
ALLOW_NON_RK3588=0
REPLACE_LANDISK=0
KEEP_SOURCE=""
PROFILE="auto"
APPLY_PROFILE=0
ORIGINAL_ARGS=("\$@")

usage() {
  cat <<'USAGE'
RemyDesk single-file installer for RK3588 ARM64 boards

Usage:
  sudo bash RemyDesk-RK3588-installer.sh [options]

Options:
  --skip-deps           Do not install Debian/Ubuntu dependencies
  --replace-landisk     Stop LAN Disk when it occupies TCP port 8010
  --keep-source DIR     Keep the extracted source tree in DIR
  --profile NAME        Select generic-rk3588 or a board profile
  --apply-profile       Reapply profile defaults during an upgrade
  --verify-only         Verify and extract-test the embedded payload only
  --allow-non-rk3588    Allow installation on another ARM64 Rockchip SoC
  -h, --help            Show this help
USAGE
}

while (( \$# )); do
  case "\$1" in
    --skip-deps) SKIP_DEPS=1 ;;
    --replace-landisk) REPLACE_LANDISK=1 ;;
    --verify-only) VERIFY_ONLY=1 ;;
    --allow-non-rk3588) ALLOW_NON_RK3588=1 ;;
    --apply-profile) APPLY_PROFILE=1 ;;
    --profile)
      shift
      [[ \$# -gt 0 ]] || { echo "--profile requires a name" >&2; exit 2; }
      PROFILE="\$1"
      ;;
    --keep-source)
      shift
      [[ \$# -gt 0 ]] || { echo "--keep-source requires a directory" >&2; exit 2; }
      KEEP_SOURCE="\$1"
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: \$1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

ARCH="\$(uname -m)"
if [[ "\$VERIFY_ONLY" -ne 1 ]]; then
  [[ "\$ARCH" == "aarch64" || "\$ARCH" == "arm64" ]] || {
    echo "Unsupported architecture: \$ARCH (ARM64 is required for installation)." >&2
    exit 1
  }
fi

COMPATIBLE=""
if [[ -r /proc/device-tree/compatible ]]; then
  COMPATIBLE="\$(tr '\0' '\n' </proc/device-tree/compatible | paste -sd, -)"
fi
if [[ "\$VERIFY_ONLY" -ne 1 && "\$ALLOW_NON_RK3588" -ne 1 && "\$COMPATIBLE" != *rk3588* ]]; then
  echo "This does not appear to be an RK3588 board: \${COMPATIBLE:-unknown compatible string}" >&2
  echo "Use --allow-non-rk3588 only if the board has compatible Rockchip DRM/RGA/MPP support." >&2
  exit 1
fi

WORKDIR="\$(mktemp -d /tmp/remydesk-installer.XXXXXX)"
cleanup() { rm -rf "\$WORKDIR"; }
trap cleanup EXIT
PAYLOAD="\$WORKDIR/payload.tar.gz"
PAYLOAD_LINE="\$(awk '/^__REMYDESK_PAYLOAD_BELOW__\$/ {print NR + 1; exit}' "\$0")"
[[ -n "\$PAYLOAD_LINE" ]] || { echo "Embedded payload marker is missing." >&2; exit 1; }
tail -n +"\$PAYLOAD_LINE" "\$0" >"\$PAYLOAD"
ACTUAL_SIZE="\$(stat -c '%s' "\$PAYLOAD")"
ACTUAL_SHA256="\$(sha256sum "\$PAYLOAD" | awk '{print \$1}')"
[[ "\$ACTUAL_SIZE" == "\$PAYLOAD_SIZE" ]] || { echo "Payload size mismatch." >&2; exit 1; }
[[ "\$ACTUAL_SHA256" == "\$PAYLOAD_SHA256" ]] || { echo "Payload checksum mismatch." >&2; exit 1; }
tar -C "\$WORKDIR" -xzf "\$PAYLOAD"
SOURCE="\$WORKDIR/$NAME"
for required in scripts/install.sh scripts/build.sh scripts/detect-profile.sh profiles/generic-rk3588/desktop.env web/index.html native/webrtc-publisher/remydesk-webrtc-publisher; do
  [[ -e "\$SOURCE/\$required" ]] || { echo "Payload is missing \$required" >&2; exit 1; }
done
echo "Payload verified: \$PAYLOAD_SHA256"

if [[ "\$VERIFY_ONLY" -eq 1 ]]; then
  echo "Installer verification passed (host=\$ARCH, target=linux/arm64, board=\${COMPATIBLE:-not-probed})."
  exit 0
fi

if [[ "\$EUID" -ne 0 ]]; then
  echo "Installation needs root privileges; restarting through sudo."
  exec sudo bash "\$0" "\${ORIGINAL_ARGS[@]}"
fi

if [[ "\$SKIP_DEPS" -ne 1 ]]; then
  command -v apt-get >/dev/null 2>&1 || {
    echo "Automatic dependency setup currently supports Debian/Ubuntu (apt)." >&2
    echo "Install the dependencies in docs/portability.md, then rerun with --skip-deps." >&2
    exit 1
  }
  export DEBIAN_FRONTEND=noninteractive
  echo "Refreshing package metadata..."
  apt-get update
  PACKAGES=(
    build-essential cmake pkg-config libdrm-dev
    ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
    network-manager dnsmasq-base iproute2 sudo
    pulseaudio-utils x11-xserver-utils ca-certificates
  )
  for vendor_pkg in librga-dev librockchip-mpp-dev gstreamer1.0-rockchip1 rockchip-multimedia-config; do
    apt-cache show "\$vendor_pkg" >/dev/null 2>&1 && PACKAGES+=("\$vendor_pkg")
  done
  apt-get install -y --no-install-recommends "\${PACKAGES[@]}"
fi

MISSING=()
for tool in cmake make pkg-config g++ ffmpeg gst-inspect-1.0 nmcli ss; do
  command -v "\$tool" >/dev/null 2>&1 || MISSING+=("command:\$tool")
done
for module in libdrm librga rockchip_mpp; do
  pkg-config --exists "\$module" 2>/dev/null || MISSING+=("pkg-config:\$module")
done
for plugin in rawvideoparse h264parse mpph264enc; do
  gst-inspect-1.0 "\$plugin" >/dev/null 2>&1 || MISSING+=("gstreamer:\$plugin")
done
if (( \${#MISSING[@]} )); then
  printf 'Missing RK3588 build/runtime capability: %s\n' "\${MISSING[@]}" >&2
  echo "The board BSP must provide matching librga, Rockchip MPP and the GStreamer MPP plugin." >&2
  echo "See docs/portability.md in the bundled source for board-specific package guidance." >&2
  exit 1
fi

if [[ -n "\$KEEP_SOURCE" ]]; then
  mkdir -p "\$KEEP_SOURCE"
  cp -a "\$SOURCE/." "\$KEEP_SOURCE/"
  echo "Source retained at \$KEEP_SOURCE"
fi

export REMYDESK_USE_BUNDLED_PUBLISHER=1
INSTALL_ARGS=()
[[ "\$REPLACE_LANDISK" -eq 1 ]] && INSTALL_ARGS+=(--replace-landisk)
[[ "\$PROFILE" != auto ]] && INSTALL_ARGS+=(--profile "\$PROFILE")
[[ "\$APPLY_PROFILE" -eq 1 ]] && INSTALL_ARGS+=(--apply-profile)
bash "\$SOURCE/scripts/install.sh" "\${INSTALL_ARGS[@]}"

echo
echo "Running RemyDesk portability checks..."
bash "\$SOURCE/scripts/remydesk-doctor.sh" || true
echo
echo "RemyDesk installation finished."
echo "Open: http://<board-ip>:8010/"
hostname -I 2>/dev/null | awk '{for (i=1; i<=NF; i++) print "  http://" \$i ":8010/"}'
echo "Desktop streaming can be enabled from the web UI after the HDMI/DRM output is ready."
exit 0

__REMYDESK_PAYLOAD_BELOW__
EOF

cat "$PAYLOAD" >>"$OUTPUT"
chmod 0755 "$OUTPUT"
(
  cd "$(dirname "$OUTPUT")"
  sha256sum "$(basename "$OUTPUT")" >"$(basename "$OUTPUT").sha256"
)

echo "Single-file installer created:"
echo "  $OUTPUT"
echo "  $OUTPUT.sha256"
echo "  embedded payload: $PAYLOAD_SIZE bytes"
