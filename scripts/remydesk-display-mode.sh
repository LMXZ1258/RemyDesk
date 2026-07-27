#!/usr/bin/env bash
set -uo pipefail

MODE="${HDMI_REQUIRED_MODE:-1920x1080}"
RATE="${HDMI_XRANDR_RATE:-60}"
OUTPUT="${HDMI_XRANDR_OUTPUT:-auto}"
DISPLAY_NAME="${HDMI_DISPLAY:-:0}"
AUTHORITY="${HDMI_XAUTHORITY:-auto}"
WAIT_SECONDS="${HDMI_MODE_WAIT_SECONDS:-30}"
FORCE_MODE="${HDMI_FORCE_MODE:-1}"

log() {
  echo "remydesk-display: $*" >&2
}

if [[ "$FORCE_MODE" == "0" || "$MODE" == "auto" ]]; then
  log "mode forcing disabled"
  exit 0
fi

find_xauthority() {
  if [[ "$AUTHORITY" != "auto" && -r "$AUTHORITY" ]]; then
    printf '%s\n' "$AUTHORITY"
    return 0
  fi

  local candidate
  candidate="$(ps -eo args | awk '
    /[X]org .* -auth / {
      for (i = 1; i <= NF; i++) {
        if ($i == "-auth" && (i + 1) <= NF) { print $(i + 1); exit }
      }
    }
  ')"
  if [[ -n "$candidate" && -r "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  for candidate in /var/run/lightdm/root/:0 /run/lightdm/root/:0 /run/user/*/gdm/Xauthority /run/user/*/Xauthority /home/*/.Xauthority; do
    [[ -r "$candidate" ]] || continue
    printf '%s\n' "$candidate"
    return 0
  done
  return 1
}

select_output() {
  local query="$1"
  if [[ "$OUTPUT" != "auto" ]]; then
    printf '%s\n' "$OUTPUT"
    return 0
  fi
  local selected
  selected="$(printf '%s\n' "$query" | awk '/^HDMI[^ ]* connected/{print $1; exit}')"
  if [[ -z "$selected" ]]; then
    selected="$(printf '%s\n' "$query" | awk '/ connected/{print $1; exit}')"
  fi
  [[ -n "$selected" ]] || return 1
  printf '%s\n' "$selected"
}

deadline=$((SECONDS + WAIT_SECONDS))
while (( SECONDS <= deadline )); do
  xauth="$(find_xauthority 2>/dev/null || true)"
  if [[ -n "$xauth" ]]; then
    query="$(DISPLAY="$DISPLAY_NAME" XAUTHORITY="$xauth" xrandr --query 2>/dev/null || true)"
    output="$(select_output "$query" 2>/dev/null || true)"
    if [[ -n "$output" ]]; then
      if DISPLAY="$DISPLAY_NAME" XAUTHORITY="$xauth" \
        xrandr --output "$output" --mode "$MODE" --rate "$RATE" --primary >/dev/null 2>&1 ||
         DISPLAY="$DISPLAY_NAME" XAUTHORITY="$xauth" \
        xrandr --output "$output" --mode "$MODE" --primary >/dev/null 2>&1; then
        current="$(DISPLAY="$DISPLAY_NAME" XAUTHORITY="$xauth" xrandr --query 2>/dev/null | awk '/ connected primary| connected/{for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+x[0-9]+\+/) {print $i; exit}}')"
        log "output=$output mode=$MODE rate=$RATE current=${current:-unknown}"
        exit 0
      fi
    fi
  fi
  sleep 1
done

log "unable to apply ${MODE}@${RATE} on display $DISPLAY_NAME after ${WAIT_SECONDS}s"
exit 0
