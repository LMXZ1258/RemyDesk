#!/usr/bin/env bash
set -u

OUT_DIR="${1:-/tmp/remydesk-dmabuf-matrix}"
mkdir -p "$OUT_DIR"
printf 'user,dma_feature,zero_copy_pkt,finished,frames,decode_errors,size_bytes\n'

run_case() {
  local user="$1" dma="$2" packet="$3"
  local name="${user}-dma${dma}-pkt${packet}"
  local stream="$OUT_DIR/$name.h264"
  local log="$OUT_DIR/$name.log"
  local decode_log="$OUT_DIR/$name.decode.log"
  local -a prefix=()
  local pid elapsed finished frames errors size

  rm -f "$stream" "$log" "$decode_log"
  if [[ "$user" == "remydesk" ]]; then
    prefix=(runuser -u remydesk --)
  fi

  setsid "${prefix[@]}" env GST_DEBUG=2 gst-launch-1.0 -e -q \
    kmssrc num-buffers=360 framerate-limit=60 do-timestamp=true \
      driver-name=rockchip dma-feature="$dma" sync-fb=false sync-vblank=true \
    '!' queue max-size-buffers=1 leaky=downstream \
    '!' mpph264enc bps=6000000 bps-min=4000000 bps-max=8000000 \
      gop=60 profile=baseline level=42 header-mode=each-idr \
      zero-copy-pkt="$packet" \
    '!' h264parse config-interval=-1 \
    '!' video/x-h264,stream-format=byte-stream,alignment=au \
    '!' filesink location="$stream" sync=false \
    >"$log" 2>&1 &
  pid=$!

  finished=0
  for elapsed in $(seq 1 150); do
    if ! kill -0 "$pid" 2>/dev/null; then
      finished=1
      break
    fi
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM -"$pid" 2>/dev/null || true
    sleep 1
    kill -KILL -"$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true

  frames=0
  errors=0
  size=0
  if [[ -s "$stream" ]]; then
    size="$(stat -c %s "$stream" 2>/dev/null || printf 0)"
    frames="$(ffprobe -v error -count_frames -select_streams v:0 \
      -show_entries stream=nb_read_frames -of default=nokey=1:noprint_wrappers=1 \
      "$stream" 2>/dev/null | tail -1)"
    [[ "$frames" =~ ^[0-9]+$ ]] || frames=0
    ffmpeg -v error -i "$stream" -f null - 2>"$decode_log" || true
    errors="$(wc -l <"$decode_log")"
  fi
  printf '%s,%s,%s,%s,%s,%s,%s\n' \
    "$user" "$dma" "$packet" "$finished" "$frames" "$errors" "$size"
  sleep 2
}

for user in root remydesk; do
  for dma in false true; do
    for packet in false true; do
      run_case "$user" "$dma" "$packet"
    done
  done
done
