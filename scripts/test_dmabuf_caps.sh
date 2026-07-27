#!/usr/bin/env bash
set -u

OUT_DIR="${1:-/tmp/remydesk-dmabuf-caps}"
PLUGIN_PATH="${REMYDESK_GST_PLUGIN_PATH:-${GST_PLUGIN_PATH:-}}"
GST_REGISTRY_FILE="${REMYDESK_GST_REGISTRY:-/tmp/gst-registry-remydesk-dmabuf.bin}"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
printf 'queue,zero_copy_pkt,finished,elapsed_ms,frames,decode_errors,size_bytes\n'

run_case() {
  local queue_mode="$1" packet="$2"
  local name="${queue_mode}-pkt${packet}"
  local stream="$OUT_DIR/$name.h264"
  local log="$OUT_DIR/$name.log"
  local decode_log="$OUT_DIR/$name.decode.log"
  local -a queue_args=() env_args=()
  local pid i finished frames errors size start_ns end_ns elapsed_ms

  case "$queue_mode" in
    none) queue_args=() ;;
    leaky1) queue_args=('!' queue max-size-buffers=1 leaky=downstream) ;;
    queue3) queue_args=('!' queue max-size-buffers=3 leaky=no) ;;
  esac

  [[ -n "$PLUGIN_PATH" ]] && env_args+=(GST_PLUGIN_PATH="$PLUGIN_PATH")
  env_args+=(GST_REGISTRY="$GST_REGISTRY_FILE" GST_DEBUG=2)

  start_ns="$(date +%s%N)"
  setsid env "${env_args[@]}" \
    gst-launch-1.0 -e -q \
    kmssrc num-buffers=360 do-timestamp=true driver-name=rockchip \
      dma-feature=true sync-fb=false sync-vblank=true \
    '!' capssetter caps='video/x-raw,framerate=60/1' join=true replace=false \
    "${queue_args[@]}" \
    '!' mpph264enc bps=6000000 bps-min=4000000 bps-max=8000000 \
      gop=60 profile=baseline level=42 header-mode=each-idr \
      zero-copy-pkt="$packet" \
    '!' h264parse config-interval=-1 \
    '!' video/x-h264,stream-format=byte-stream,alignment=au \
    '!' filesink location="$stream" sync=false \
    >"$log" 2>&1 &
  pid=$!

  finished=0
  for i in $(seq 1 150); do
    if ! kill -0 "$pid" 2>/dev/null; then finished=1; break; fi
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM -"$pid" 2>/dev/null || true
    sleep 1
    kill -KILL -"$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
  end_ns="$(date +%s%N)"
  elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

  frames=0; errors=0; size=0
  if [[ -s "$stream" ]]; then
    size="$(stat -c %s "$stream")"
    frames="$(ffprobe -v error -count_frames -select_streams v:0 \
      -show_entries stream=nb_read_frames -of default=nokey=1:noprint_wrappers=1 \
      "$stream" 2>/dev/null | tail -1)"
    [[ "$frames" =~ ^[0-9]+$ ]] || frames=0
    ffmpeg -v error -i "$stream" -f null - 2>"$decode_log" || true
    errors="$(wc -l <"$decode_log")"
  fi
  printf '%s,%s,%s,%s,%s,%s,%s\n' \
    "$queue_mode" "$packet" "$finished" "$elapsed_ms" "$frames" "$errors" "$size"
  sleep 2
}

for queue_mode in none; do
  for packet in false true; do
    run_case "$queue_mode" "$packet"
  done
done
