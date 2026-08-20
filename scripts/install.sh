#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="/opt/remydesk"
REPLACE_LANDISK=0
PROFILE="auto"
APPLY_PROFILE=0

usage() {
  cat <<'EOF'
Usage: sudo ./scripts/install.sh [options]

Options:
  --replace-landisk       Stop LAN Disk if it occupies TCP 8010
  --profile NAME          Select a profile from profiles/ (default: auto)
  --apply-profile         Apply profile defaults even when desktop.env exists
  -h, --help              Show help
EOF
}

while (( $# )); do
  case "$1" in
    --replace-landisk) REPLACE_LANDISK=1 ;;
    --profile)
      shift
      [[ $# -gt 0 ]] || { echo "--profile requires a name" >&2; exit 2; }
      PROFILE="$1"
      ;;
    --apply-profile) APPLY_PROFILE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
if [[ "$EUID" -ne 0 ]]; then
  args=()
  [[ "$REPLACE_LANDISK" -eq 1 ]] && args+=(--replace-landisk)
  [[ "$PROFILE" != auto ]] && args+=(--profile "$PROFILE")
  [[ "$APPLY_PROFILE" -eq 1 ]] && args+=(--apply-profile)
  exec sudo "$0" "${args[@]}"
fi

if [[ "$PROFILE" == auto ]]; then
  PROFILE="$($ROOT/scripts/detect-profile.sh)"
fi
PROFILE_DIR="$ROOT/profiles/$PROFILE"
[[ -f "$PROFILE_DIR/profile.conf" && -f "$PROFILE_DIR/desktop.env" ]] || {
  echo "Unknown or incomplete RemyDesk profile: $PROFILE" >&2
  echo "Available profiles:" >&2
  find "$ROOT/profiles" -mindepth 1 -maxdepth 1 -type d -printf '  %f\n' >&2
  exit 1
}
echo "Using RemyDesk profile: $PROFILE"

for tool in cmake make pkg-config g++ install; do
  command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done
pkg-config --exists libdrm || { echo "missing libdrm development files" >&2; exit 1; }
pkg-config --exists librga || { echo "missing librga development files" >&2; exit 1; }
pkg-config --exists rockchip_mpp || { echo "missing Rockchip MPP development files" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 ||
  echo "warning: FFmpeg is missing; kmsgrab and the software fallback are unavailable" >&2
if ! { command -v gst-inspect-1.0 >/dev/null 2>&1 &&
       gst-inspect-1.0 rawvideoparse >/dev/null 2>&1 &&
       gst-inspect-1.0 h264parse >/dev/null 2>&1 &&
       gst-inspect-1.0 mpph264enc >/dev/null 2>&1; }; then
  echo "warning: GStreamer rawvideoparse/h264parse/mpph264enc is incomplete; hardware encoding may be unavailable" >&2
fi

if ss -ltnH 'sport = :8010' | grep -q .; then
  if systemctl is-active --quiet remydesk.service 2>/dev/null; then
    echo "Stopping the existing RemyDesk service for upgrade."
    systemctl stop remydesk.service
  elif [[ "$REPLACE_LANDISK" -eq 1 ]]; then
    systemctl stop lan-disk.service 2>/dev/null || true
    systemctl --user --machine="${SUDO_USER:-root}@" stop lan-disk.service 2>/dev/null || true
  else
    echo "TCP 8010 is in use. Stop LAN Disk first or run: sudo $0 --replace-landisk" >&2
    exit 1
  fi
fi

if [[ "${REMYDESK_SKIP_BUILD:-0}" == "1" ]]; then
  for artifact in \
    "$ROOT/build/remydeskd" \
    "$ROOT/native/desktop-streamer/drm_fb_probe" \
    "$ROOT/native/desktop-streamer/drm_rga_mpp_stream" \
    "$ROOT/native/webrtc-publisher/remydesk-webrtc-publisher"; do
    [[ -x "$artifact" ]] || { echo "missing prebuilt artifact: $artifact" >&2; exit 1; }
  done
else
  REMYDESK_BUILD_PROFILE="$PROFILE" "$ROOT/scripts/build.sh"
fi

getent group remydesk >/dev/null || groupadd --system remydesk
for group in video render input; do
  getent group "$group" >/dev/null || groupadd --system "$group"
done
id remydesk >/dev/null 2>&1 || useradd --system --gid remydesk --home-dir /var/lib/remydesk --shell /usr/sbin/nologin remydesk
for group in video render input; do
  usermod -a -G "$group" remydesk
done

install -d -m 0755 "$PREFIX/bin" "$PREFIX/libexec" "$PREFIX/share/web" /etc/remydesk
install -d -o remydesk -g remydesk -m 0750 /var/lib/remydesk /srv/remydesk/data
install -m 0755 "$ROOT/build/remydeskd" "$PREFIX/bin/remydeskd"
install -m 0755 "$ROOT/native/webrtc-publisher/remydesk-webrtc-publisher" "$PREFIX/bin/remydesk-webrtc-publisher"
install -m 0755 "$ROOT/native/desktop-streamer/drm_fb_probe" "$PREFIX/libexec/drm_fb_probe"
install -m 0755 "$ROOT/native/desktop-streamer/drm_rga_mpp_stream" "$PREFIX/libexec/drm_rga_mpp_stream"
install -m 0755 "$ROOT/native/desktop-streamer/drm_hotplug_stream.sh" "$PREFIX/libexec/drm_hotplug_stream.sh"
install -m 0755 "$ROOT/native/desktop-streamer/portable_h264_stream.sh" "$PREFIX/libexec/portable_h264_stream.sh"
install -m 0755 "$ROOT/scripts/remydesk-display-mode.sh" "$PREFIX/libexec/remydesk-display-mode.sh"
install -m 0755 "$ROOT/scripts/remydesk-doctor.sh" "$PREFIX/libexec/remydesk-doctor.sh"
install -m 0755 "$ROOT/scripts/detect-profile.sh" "$PREFIX/libexec/detect-profile.sh"
if [[ "$PROFILE" == "firefly-rk3399" ]]; then
  RK3399_RGA="$ROOT/third_party/librga/rk3399/librga.so.2"
  if [[ ! -f "$RK3399_RGA" ]]; then
    "$ROOT/scripts/fetch-rk3399-librga.sh" "$RK3399_RGA"
  fi
  printf '%s  %s\n' \
    3da1413445885420abf00821640ec8a37289ec176fe4ffeee0de5f68418ed50e \
    "$RK3399_RGA" | sha256sum -c -
  install -d -m 0755 "$PREFIX/lib/rga-rk3399"
  install -m 0755 "$RK3399_RGA" "$PREFIX/lib/rga-rk3399/librga.so.2"
  ln -sfn librga.so.2 "$PREFIX/lib/rga-rk3399/librga.so"
fi
install -d -m 0755 "$PREFIX/share/profiles"
cp -a "$ROOT/profiles/." "$PREFIX/share/profiles/"
cp -a "$ROOT/web/." "$PREFIX/share/web/"
chown -R root:root "$PREFIX/share/web"
find "$PREFIX/share/web" -type d -exec chmod 0755 {} +
find "$PREFIX/share/web" -type f -exec chmod 0644 {} +

# NetworkManager's ipv4.method=shared starts its own dnsmasq instance for the
# hotspot interface. A separately enabled system dnsmasq binds port 53 first,
# fails against systemd-resolved, and is not used by RemyDesk.
if systemctl list-unit-files dnsmasq.service >/dev/null 2>&1; then
  systemctl disable --now dnsmasq.service >/dev/null 2>&1 || true
  systemctl reset-failed dnsmasq.service >/dev/null 2>&1 || true
fi

[[ -e /etc/remydesk/remydesk.env ]] || install -m 0640 "$ROOT/config/remydesk.env.example" /etc/remydesk/remydesk.env
DESKTOP_CONFIG_NEW=0
if [[ ! -e /etc/remydesk/desktop.env ]]; then
  install -m 0640 "$ROOT/config/desktop.env.example" /etc/remydesk/desktop.env
  DESKTOP_CONFIG_NEW=1
fi

apply_env_defaults() {
  local source="$1" line key value escaped
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" == \#* || "$line" != *=* ]] && continue
    key="${line%%=*}"
    value="${line#*=}"
    [[ "$key" =~ ^[A-Z0-9_]+$ ]] || continue
    if grep -q "^${key}=" /etc/remydesk/desktop.env; then
      [[ "$APPLY_PROFILE" -eq 1 ]] || continue
      escaped="${value//&/\\&}"
      sed -i "s|^${key}=.*|${key}=${escaped}|" /etc/remydesk/desktop.env
    else
      printf '%s=%s\n' "$key" "$value" >>/etc/remydesk/desktop.env
    fi
  done <"$source"
}

if [[ "$DESKTOP_CONFIG_NEW" -eq 1 || "$APPLY_PROFILE" -eq 1 ]]; then
  apply_env_defaults "$PROFILE_DIR/desktop.env"
fi
printf '%s\n' "$PROFILE" >/etc/remydesk/profile
ensure_desktop_default() {
  local key="$1"
  local value="$2"
  grep -q "^${key}=" /etc/remydesk/desktop.env || printf '%s=%s\n' "$key" "$value" >>/etc/remydesk/desktop.env
}
ensure_desktop_default REMYDESK_VIDEO_WIDTH 1920
ensure_desktop_default REMYDESK_VIDEO_HEIGHT 1080
ensure_desktop_default REMYDESK_MPP_ZERO_COPY_PACKET false
ensure_desktop_default REMYDESK_MPP_USER remydesk
ensure_desktop_default H264_NAL_QUEUE 8
ensure_desktop_default AUDIO_THREAD_QUEUE_SIZE 32
ensure_desktop_default HDMI_REQUIRED_MODE 1920x1080
ensure_desktop_default HDMI_FORCE_MODE 1
ensure_desktop_default HDMI_XRANDR_OUTPUT auto
ensure_desktop_default HDMI_XRANDR_RATE 60
ensure_desktop_default HDMI_DISPLAY :0
ensure_desktop_default HDMI_XAUTHORITY auto
ensure_desktop_default REMYDESK_DRM_FORCE_CONNECTOR 0
ensure_desktop_default REMYDESK_DRM_CONNECTOR_STATUS auto
ensure_desktop_default REMYDESK_DRM_CONNECTOR_SETTLE_SECONDS 2
chown root:remydesk /etc/remydesk/remydesk.env
chown root:remydesk /etc/remydesk/desktop.env
chmod 0640 /etc/remydesk/remydesk.env /etc/remydesk/desktop.env
chmod 0644 /etc/remydesk/profile
install -m 0644 "$ROOT/packaging/systemd/remydesk.service" /etc/systemd/system/remydesk.service
install -m 0644 "$ROOT/packaging/systemd/remydesk-desktop.service" /etc/systemd/system/remydesk-desktop.service
install -m 0644 "$ROOT/packaging/systemd/remydesk-display-mode.service" /etc/systemd/system/remydesk-display-mode.service
install -m 0644 "$ROOT/packaging/tmpfiles/remydesk.conf" /etc/tmpfiles.d/remydesk.conf
install -m 0644 "$ROOT/packaging/udev/70-remydesk.rules" /etc/udev/rules.d/70-remydesk.rules
install -m 0440 "$ROOT/packaging/sudoers/remydesk" /etc/sudoers.d/remydesk
visudo -cf /etc/sudoers.d/remydesk >/dev/null

systemd-tmpfiles --create /etc/tmpfiles.d/remydesk.conf
udevadm control --reload-rules
udevadm trigger --subsystem-match=drm || true
systemctl daemon-reload
systemctl enable remydesk-display-mode.service
systemctl restart remydesk-display-mode.service || true
systemctl disable --now remydesk-desktop.service 2>/dev/null || true
systemctl enable --now remydesk.service

echo "Installed RemyDesk. Open http://<device-ip>:8010/"
echo "Hardware profile: $PROFILE"
echo "Desktop streaming remains disabled until enabled from the web UI."
