#!/usr/bin/env bash
set -u

video_probe=0
if [[ "${1:-}" == "--video" ]]; then
  video_probe=1
fi

pass=0
warn=0
check() {
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then printf '[OK]   %s\n' "$label"; pass=$((pass + 1));
  else printf '[WARN] %s\n' "$label"; warn=$((warn + 1)); fi
}

has_drm_card() {
  compgen -G '/dev/dri/card[0-9]*' >/dev/null
}

ffmpeg_has_kmsgrab() {
  command -v ffmpeg >/dev/null 2>&1 &&
    ffmpeg -hide_banner -demuxers 2>/dev/null | grep -E '[[:space:]]kmsgrab[[:space:]]' >/dev/null
}

gst_has() {
  command -v gst-inspect-1.0 >/dev/null 2>&1 && gst-inspect-1.0 "$1" >/dev/null 2>&1
}

echo "RemyDesk doctor"
if [[ -r /etc/remydesk/profile ]]; then
  printf '[INFO] hardware profile: %s\n' "$(head -n 1 /etc/remydesk/profile)"
elif [[ -x /opt/remydesk/libexec/detect-profile.sh ]]; then
  printf '[INFO] detected profile: %s\n' "$(/opt/remydesk/libexec/detect-profile.sh)"
fi
check "supported Rockchip device tree" grep -Eqi 'rk(3399|3588)' /proc/device-tree/compatible
check "NetworkManager nmcli" command -v nmcli
check "DRM device" has_drm_card
check "uinput device" test -e /dev/uinput
check "libdrm" pkg-config --exists libdrm
check "Rockchip RGA" pkg-config --exists librga
check "Rockchip MPP" pkg-config --exists rockchip_mpp
check "FFmpeg kmsgrab" ffmpeg_has_kmsgrab
check "GStreamer MPP encoder" gst_has mpph264enc
check "GStreamer raw parser" gst_has rawvideoparse
check "C++ control plane" test -x /opt/remydesk/bin/remydeskd
check "LAN WebRTC publisher" test -x /opt/remydesk/bin/remydesk-webrtc-publisher
check "desktop encoder" test -x /opt/remydesk/libexec/drm_rga_mpp_stream
check "portable video selector" test -x /opt/remydesk/libexec/portable_h264_stream.sh
check "display mode helper" test -x /opt/remydesk/libexec/remydesk-display-mode.sh
check "display mode service" systemctl is-active remydesk-display-mode.service
check "RemyDesk service" systemctl is-active remydesk.service

if [[ -r /run/remydesk/video-backend-v2 ]]; then
  printf '[INFO] cached video backend: %s\n' "$(head -n 1 /run/remydesk/video-backend-v2)"
fi

if (( video_probe )); then
  printf '[INFO] running a decoded video-backend stability probe...\n'
  if [[ -x /opt/remydesk/libexec/portable_h264_stream.sh ]]; then
    if selected="$(cd /opt/remydesk/libexec && ./portable_h264_stream.sh --probe)"; then
      printf '[OK]   selected video backend: %s\n' "$selected"
      pass=$((pass + 1))
    else
      printf '[WARN] video backend stability probe failed\n'
      warn=$((warn + 1))
    fi
  else
    printf '[WARN] portable video selector is not installed\n'
    warn=$((warn + 1))
  fi
fi

if [[ -x /opt/remydesk/bin/remydeskd ]]; then
  /opt/remydesk/bin/remydeskd --probe 2>/dev/null || true
fi
printf '\nResult: %d passed, %d warnings\n' "$pass" "$warn"
exit "$((warn > 0))"
