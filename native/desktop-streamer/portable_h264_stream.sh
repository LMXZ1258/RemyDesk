#!/usr/bin/env bash
set -uo pipefail

BACKEND="${REMYDESK_VIDEO_BACKEND:-auto}"
DRM_DEVICE="${REMYDESK_DRM_DEVICE:-auto}"
DRM_DRIVER="${REMYDESK_DRM_DRIVER:-rockchip}"
FPS="${REMYDESK_VIDEO_FPS:-30}"
BITRATE="${REMYDESK_VIDEO_BITRATE:-6000000}"
OUTPUT_WIDTH="${REMYDESK_VIDEO_WIDTH:-${REMYDESK_SOFTWARE_WIDTH:-1280}}"
OUTPUT_HEIGHT="${REMYDESK_VIDEO_HEIGHT:-${REMYDESK_SOFTWARE_HEIGHT:-720}}"
H264_PROFILE="${REMYDESK_H264_PROFILE:-baseline}"
H264_LEVEL="${REMYDESK_H264_LEVEL:-40}"
H264_CABAC="${REMYDESK_H264_CABAC:-0}"
PROBE_SECONDS="${REMYDESK_VIDEO_PROBE_SECONDS:-2}"
MPP_ZERO_COPY_PACKET="${REMYDESK_MPP_ZERO_COPY_PACKET:-false}"
MPP_RUNTIME_USER="${REMYDESK_MPP_USER:-remydesk}"
NATIVE_MPP_ZERO_COPY="${REMYDESK_NATIVE_MPP_ZERO_COPY:-0}"
CACHE_FILE="${REMYDESK_VIDEO_CACHE_FILE:-/run/remydesk/video-backend-v2}"
PROBE_ONLY=0

if [[ "${1:-}" == "--probe" ]]; then
  PROBE_ONLY=1
fi

log() {
  echo "remydesk-video: $*" >&2
}

prepare_display_mode() {
  if [[ "${HDMI_FORCE_MODE:-0}" != "0" && -x ./remydesk-display-mode.sh ]]; then
    HDMI_MODE_WAIT_SECONDS="${HDMI_STREAM_MODE_WAIT_SECONDS:-5}" ./remydesk-display-mode.sh || true
  fi
}

find_drm_device() {
  if [[ "$DRM_DEVICE" != "auto" && -e "$DRM_DEVICE" ]]; then
    return 0
  fi

  local candidate
  for candidate in /dev/dri/card[0-9]*; do
    [[ -e "$candidate" ]] || continue
    if [[ -x ./drm_fb_probe ]] && ./drm_fb_probe "$candidate" 2>/dev/null | awk '
      /connector .* connected/ { connected = 1 }
      /crtc [0-9]+: fb=[1-9][0-9]* .*size=/ { framebuffer = 1 }
      END { exit !(connected && framebuffer) }
    '; then
      DRM_DEVICE="$candidate"
      return 0
    fi
  done

  for candidate in /dev/dri/card[0-9]*; do
    [[ -e "$candidate" ]] || continue
    DRM_DEVICE="$candidate"
    return 0
  done
  return 1
}

gst_has_element() {
  command -v gst-inspect-1.0 >/dev/null 2>&1 &&
    gst-inspect-1.0 "$1" >/dev/null 2>&1
}

gst_has_property() {
  # Do not use grep -q here. With pipefail enabled, gst-inspect can receive
  # SIGPIPE after an early grep exit and make an existing property look absent.
  gst-inspect-1.0 "$1" 2>/dev/null |
    grep -E "^[[:space:]]*$2[[:space:]]*:" >/dev/null
}

build_mpp_run_prefix() {
  MPP_RUN_PREFIX=()
  # The Orange Pi 1.2.0 image ships a gst-rockchip build that emits corrupt
  # packets when mpph264enc runs as uid 0, while the same encoder is stable for
  # an ordinary member of the video/render groups. Keep KMS capture privileged
  # and drop privileges only for the raw-NV12 MPP encoder process.
  if [[ "$EUID" -eq 0 ]] && command -v runuser >/dev/null 2>&1 && id "$MPP_RUNTIME_USER" >/dev/null 2>&1; then
    MPP_RUN_PREFIX=(runuser -u "$MPP_RUNTIME_USER" --)
  fi
}

build_mpp_encoder() {
  MPP_ENCODER=(
    mpph264enc
    "bps=$BITRATE"
    "bps-min=$((BITRATE * 2 / 3))"
    "bps-max=$((BITRATE * 4 / 3))"
    "gop=$FPS"
    "profile=$H264_PROFILE"
    "level=$H264_LEVEL"
  )
  if gst_has_property mpph264enc header-mode; then
    MPP_ENCODER+=(header-mode=each-idr)
  fi
  if gst_has_property mpph264enc zero-copy-pkt; then
    MPP_ENCODER+=("zero-copy-pkt=$MPP_ZERO_COPY_PACKET")
  fi
}

build_direct_gstreamer_pipeline() {
  GST_DIRECT_PIPELINE=(
    kmssrc
    "framerate-limit=$FPS"
    do-timestamp=true
  )
  if [[ -n "$DRM_DRIVER" && "$DRM_DRIVER" != "auto" ]]; then
    GST_DIRECT_PIPELINE+=("driver-name=$DRM_DRIVER")
  fi
  if gst_has_property kmssrc dma-feature; then
    GST_DIRECT_PIPELINE+=("dma-feature=${REMYDESK_GST_DMA_FEATURE:-true}")
  fi
  if gst_has_property kmssrc sync-fb; then
    # Static desktops do not necessarily flip the FB. Waiting for a flip can
    # leave the pipeline negotiated but emitting no frames.
    GST_DIRECT_PIPELINE+=("sync-fb=${REMYDESK_GST_SYNC_FB:-false}")
  fi
  if gst_has_property kmssrc sync-vblank; then
    GST_DIRECT_PIPELINE+=("sync-vblank=${REMYDESK_GST_SYNC_VBLANK:-true}")
  fi

  build_mpp_encoder
  GST_DIRECT_PIPELINE+=(
    "!" queue max-size-buffers=1 leaky=downstream
    "!" "${MPP_ENCODER[@]}"
    "!" h264parse config-interval=-1
    "!" video/x-h264,stream-format=byte-stream,alignment=au
  )
}

build_raw_mpp_pipeline() {
  build_mpp_encoder
  GST_RAW_MPP_PIPELINE=(
    gst-launch-1.0 -e -q
    fdsrc
    "!" rawvideoparse format=nv12 "width=$OUTPUT_WIDTH" "height=$OUTPUT_HEIGHT" "framerate=$FPS/1"
    "!" queue max-size-buffers=1 leaky=downstream
    "!" "${MPP_ENCODER[@]}"
    "!" h264parse config-interval=-1
    "!" video/x-h264,stream-format=byte-stream,alignment=au
  )
}

validate_h264() {
  local stream="$1"
  local decode_log="$2"
  local error_lines

  [[ -s "$stream" ]] || return 1
  command -v ffmpeg >/dev/null 2>&1 || return 0
  ffmpeg -v error -i "$stream" -f null - 2>"$decode_log" || true
  error_lines="$(wc -l <"$decode_log")"
  if (( error_lines > 4 )); then
    log "H.264 stability probe produced $error_lines decoder errors"
    head -n 8 "$decode_log" >&2
    return 1
  fi
  return 0
}

direct_gstreamer_mpp_available() {
  gst_has_element kmssrc || return 1
  gst_has_element mpph264enc || return 1
  gst_has_element h264parse || return 1
  build_direct_gstreamer_pipeline

  local probe_dir probe_log probe_stream decode_log
  probe_dir="$(mktemp -d)" || return 1
  probe_log="$probe_dir/pipeline.log"
  probe_stream="$probe_dir/probe.h264"
  decode_log="$probe_dir/decode.log"

  timeout "$PROBE_SECONDS" gst-launch-1.0 -v \
    "${GST_DIRECT_PIPELINE[@]}" \
    "!" filesink "location=$probe_stream" sync=false \
    >"$probe_log" 2>&1 || true

  if ! grep -E 'MppH264Enc:.*src: caps = video/x-h264' "$probe_log" >/dev/null ||
     [[ ! -s "$probe_stream" ]]; then
    log "direct KMS/MPP probe negotiated but emitted no usable video"
    tail -n 20 "$probe_log" >&2
    rm -rf "$probe_dir"
    return 1
  fi
  if ! validate_h264 "$probe_stream" "$decode_log"; then
    rm -rf "$probe_dir"
    return 1
  fi

  if grep -E 'MppH264Enc:.*sink: caps = video/x-raw\\(memory:DMABuf\\)' "$probe_log" >/dev/null; then
    GST_INPUT_MODE="dmabuf"
  else
    GST_INPUT_MODE="mapped"
  fi
  rm -rf "$probe_dir"
  return 0
}

raw_mpp_available() {
  command -v ffmpeg >/dev/null 2>&1 || return 1
  gst_has_element rawvideoparse || return 1
  gst_has_element mpph264enc || return 1
  gst_has_element h264parse || return 1
  find_drm_device || return 1
  build_mpp_encoder
  build_mpp_run_prefix

  local probe_dir probe_stream decode_log
  probe_dir="$(mktemp -d)" || return 1
  probe_stream="$probe_dir/probe.h264"
  decode_log="$probe_dir/decode.log"
  if (( ${#MPP_RUN_PREFIX[@]} )); then
    chown "$MPP_RUNTIME_USER" "$probe_dir"
  fi

  "${MPP_RUN_PREFIX[@]}" timeout 10 gst-launch-1.0 -e -q \
    videotestsrc num-buffers=180 \
    "!" "video/x-raw,format=NV12,width=$OUTPUT_WIDTH,height=$OUTPUT_HEIGHT,framerate=$FPS/1" \
    "!" "${MPP_ENCODER[@]}" \
    "!" h264parse config-interval=-1 \
    "!" video/x-h264,stream-format=byte-stream,alignment=au \
    "!" filesink "location=$probe_stream" sync=false \
    >/dev/null 2>&1 || true

  if ! validate_h264 "$probe_stream" "$decode_log"; then
    log "Rockchip MPP raw-NV12 probe failed"
    rm -rf "$probe_dir"
    return 1
  fi
  rm -rf "$probe_dir"
  return 0
}

run_direct_gstreamer_mpp() {
  build_direct_gstreamer_pipeline
  log "backend=gstreamer-mpp capture=kms conversion=mpp encoder=rockchip-mpp input=${GST_INPUT_MODE:-auto} packet_zero_copy=$MPP_ZERO_COPY_PACKET"
  exec gst-launch-1.0 -e -q \
    "${GST_DIRECT_PIPELINE[@]}" \
    "!" fdsink fd=1 sync=false
}

hybrid_ffmpeg_pid=""
hybrid_gst_pid=""
hybrid_tmpdir=""
hybrid_cleanup() {
  if [[ -n "$hybrid_gst_pid" ]] && kill -0 "$hybrid_gst_pid" 2>/dev/null; then
    kill "$hybrid_gst_pid" 2>/dev/null || true
  fi
  if [[ -n "$hybrid_ffmpeg_pid" ]] && kill -0 "$hybrid_ffmpeg_pid" 2>/dev/null; then
    kill "$hybrid_ffmpeg_pid" 2>/dev/null || true
  fi
  [[ -n "$hybrid_gst_pid" ]] && wait "$hybrid_gst_pid" 2>/dev/null || true
  [[ -n "$hybrid_ffmpeg_pid" ]] && wait "$hybrid_ffmpeg_pid" 2>/dev/null || true
  [[ -n "$hybrid_tmpdir" ]] && rm -rf "$hybrid_tmpdir"
  hybrid_ffmpeg_pid=""
  hybrid_gst_pid=""
  hybrid_tmpdir=""
}

run_ffmpeg_gstreamer_mpp() {
  find_drm_device || {
    log "no usable DRM card found"
    return 1
  }
  build_raw_mpp_pipeline
  build_mpp_run_prefix
  hybrid_tmpdir="$(mktemp -d)" || return 1
  local fifo="$hybrid_tmpdir/nv12.fifo"
  mkfifo "$fifo"
  if (( ${#MPP_RUN_PREFIX[@]} )); then
    chown -R "$MPP_RUNTIME_USER" "$hybrid_tmpdir"
  fi
  trap 'hybrid_cleanup; exit 0' INT TERM
  trap hybrid_cleanup EXIT

  log "backend=ffmpeg-gstreamer-mpp capture=kmsgrab conversion=cpu-nv12 encoder=rockchip-mpp drm_device=$DRM_DEVICE output=${OUTPUT_WIDTH}x${OUTPUT_HEIGHT} fps=$FPS bitrate=$BITRATE packet_zero_copy=$MPP_ZERO_COPY_PACKET"
  ffmpeg \
    -hide_banner -loglevel warning -y \
    -fflags nobuffer -flags low_delay \
    -f kmsgrab -device "$DRM_DEVICE" -framerate "$FPS" -i - \
    -vf "hwdownload,format=bgr0,scale=${OUTPUT_WIDTH}:${OUTPUT_HEIGHT}:flags=fast_bilinear,format=nv12" \
    -an -pix_fmt nv12 -f rawvideo "$fifo" &
  hybrid_ffmpeg_pid="$!"

  "${MPP_RUN_PREFIX[@]}" "${GST_RAW_MPP_PIPELINE[@]}" \
    "!" fdsink fd=1 sync=false \
    <"$fifo" &
  hybrid_gst_pid="$!"

  local rc=0
  wait "$hybrid_gst_pid" || rc="$?"
  hybrid_cleanup
  trap - EXIT INT TERM
  return "$rc"
}

run_ffmpeg_software() {
  command -v ffmpeg >/dev/null 2>&1 || {
    log "ffmpeg fallback is unavailable"
    exit 1
  }
  find_drm_device || {
    log "no usable DRM card found"
    exit 1
  }

  log "backend=ffmpeg-software capture=kmsgrab conversion=cpu encoder=libx264 drm_device=$DRM_DEVICE output=${OUTPUT_WIDTH}x${OUTPUT_HEIGHT} fps=$FPS bitrate=$BITRATE"
  exec ffmpeg \
    -hide_banner -loglevel warning \
    -fflags nobuffer -flags low_delay \
    -f kmsgrab -device "$DRM_DEVICE" -framerate "$FPS" -i - \
    -vf "hwdownload,format=bgr0,scale=${OUTPUT_WIDTH}:${OUTPUT_HEIGHT}:flags=fast_bilinear,format=yuv420p" \
    -an -c:v libx264 -preset ultrafast -tune zerolatency \
    -profile:v "$H264_PROFILE" -level "${H264_LEVEL:0:1}.${H264_LEVEL:1}" -g "$FPS" -keyint_min "$FPS" -sc_threshold 0 \
    -b:v "$BITRATE" -maxrate "$BITRATE" -bufsize "$((BITRATE / 4))" \
    -x264-params "sliced-threads=0:sync-lookahead=0:rc-lookahead=0:cabac=$H264_CABAC" \
    -flush_packets 1 -f h264 -
}

run_native_mpp() {
  [[ -x ./drm_hotplug_stream.sh ]] || {
    log "native MPP backend is unavailable"
    exit 1
  }
  local native_args=(
    -f "$FPS" -b "$BITRATE"
    --out-width "$OUTPUT_WIDTH" --out-height "$OUTPUT_HEIGHT"
    --color default --yuv nv12
    --h264-profile "$H264_PROFILE" --h264-level "$H264_LEVEL" --h264-cabac "$H264_CABAC"
    --no-cursor
  )
  if [[ "$NATIVE_MPP_ZERO_COPY" == "0" ]]; then
    native_args+=(--cpu-stage)
  fi
  if [[ "${REMYDESK_NATIVE_WAIT_VBLANK:-1}" == "0" ]]; then
    native_args+=(--no-wait-vblank)
  fi
  if [[ -n "${REMYDESK_NATIVE_BUFFER_COUNT:-}" ]]; then
    native_args+=(--buffer-count "$REMYDESK_NATIVE_BUFFER_COUNT")
  fi
  log "backend=native-mpp zero_copy=$NATIVE_MPP_ZERO_COPY fps=$FPS bitrate=$BITRATE h264=$H264_PROFILE/$H264_LEVEL cabac=$H264_CABAC"
  exec ./drm_hotplug_stream.sh "${native_args[@]}"
}

cache_backend() {
  local backend="$1"
  local cache_dir
  cache_dir="$(dirname "$CACHE_FILE")"
  mkdir -p "$cache_dir" 2>/dev/null || true
  if printf '%s\n' "$backend" >"$CACHE_FILE.tmp.$$" 2>/dev/null; then
    mv -f "$CACHE_FILE.tmp.$$" "$CACHE_FILE"
  fi
}

cached_backend() {
  [[ -r "$CACHE_FILE" ]] || return 1
  local value
  value="$(head -n 1 "$CACHE_FILE")"
  case "$value" in
    gstreamer-mpp)
      gst_has_element kmssrc && gst_has_element mpph264enc || return 1
      ;;
    ffmpeg-gstreamer-mpp)
      command -v ffmpeg >/dev/null 2>&1 && gst_has_element rawvideoparse && gst_has_element mpph264enc || return 1
      ;;
    ffmpeg-software)
      command -v ffmpeg >/dev/null 2>&1 || return 1
      ;;
    *) return 1 ;;
  esac
  printf '%s\n' "$value"
}

select_auto_backend() {
  local selected
  if selected="$(cached_backend)"; then
    log "using cached backend=$selected cache=$CACHE_FILE"
    printf '%s\n' "$selected"
    return 0
  fi
  # Prefer the portable raw-NV12 bridge. On several vendor BSPs a failed
  # kmssrc -> mpph264enc probe leaves the MPP service unsettled for a moment
  # and can corrupt the next encoder session. Direct DMA-BUF remains available
  # as an explicit backend and as a fallback when FFmpeg is unavailable.
  if raw_mpp_available; then
    selected="ffmpeg-gstreamer-mpp"
  elif direct_gstreamer_mpp_available; then
    selected="gstreamer-mpp"
  else
    selected="ffmpeg-software"
  fi
  cache_backend "$selected"
  log "selected backend=$selected cache=$CACHE_FILE"
  printf '%s\n' "$selected"
}

run_selected_backend() {
  case "$1" in
    gstreamer-mpp) run_direct_gstreamer_mpp ;;
    ffmpeg-gstreamer-mpp) run_ffmpeg_gstreamer_mpp ;;
    ffmpeg-software) run_ffmpeg_software ;;
    native-mpp) run_native_mpp ;;
    *) log "internal error: unknown selected backend=$1"; exit 2 ;;
  esac
}

selected=""
if (( ! PROBE_ONLY )); then
  prepare_display_mode
fi
case "$BACKEND" in
  auto)
    selected="$(select_auto_backend)"
    ;;
  gstreamer-mpp|direct-mpp)
    direct_gstreamer_mpp_available || {
      log "requested direct GStreamer MPP backend failed its stability probe"
      exit 1
    }
    selected="gstreamer-mpp"
    ;;
  ffmpeg-gstreamer-mpp|hybrid-mpp)
    raw_mpp_available || {
      log "requested FFmpeg/GStreamer MPP backend failed its capability probe"
      exit 1
    }
    selected="ffmpeg-gstreamer-mpp"
    ;;
  mpp|hardware)
    if direct_gstreamer_mpp_available; then
      selected="gstreamer-mpp"
    elif raw_mpp_available; then
      selected="ffmpeg-gstreamer-mpp"
    else
      log "no stable Rockchip MPP backend is available"
      exit 1
    fi
    ;;
  ffmpeg-software|software)
    selected="ffmpeg-software"
    ;;
  native-mpp)
    selected="native-mpp"
    ;;
  *)
    log "unknown REMYDESK_VIDEO_BACKEND=$BACKEND"
    exit 2
    ;;
esac

if (( PROBE_ONLY )); then
  log "probe selected backend=$selected"
  printf '%s\n' "$selected"
  exit 0
fi

run_selected_backend "$selected"
