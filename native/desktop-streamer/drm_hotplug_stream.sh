#!/usr/bin/env bash
set -u

CARD="${CARD:-auto}"
WAIT_SECONDS="${HDMI_WAIT_SECONDS:-2}"
RESTART_SECONDS="${HDMI_RESTART_SECONDS:-1}"
REQUIRED_MODE="${HDMI_REQUIRED_MODE:-auto}"
XRANDR_OUTPUT="${HDMI_XRANDR_OUTPUT:-auto}"
XRANDR_RATE="${HDMI_XRANDR_RATE:-60}"
FORCE_SECONDS="${HDMI_FORCE_SECONDS:-3}"
DISPLAY_NAME="${HDMI_DISPLAY:-:0}"

args=("$@")
child_pid=""
has_card_arg=0
last_wait_log=0
last_force_log=0
last_force_run=0

for ((i = 0; i < ${#args[@]}; i++)); do
  if [[ "${args[$i]}" == "-c" && $((i + 1)) -lt ${#args[@]} ]]; then
    CARD="${args[$((i + 1))]}"
    has_card_arg=1
  fi
done

cleanup() {
  if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
}

request_idr() {
  if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill -USR1 "$child_pid" 2>/dev/null || true
  fi
}

trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM
trap request_idr USR1

has_active_hdmi_fb() {
  if [[ "$CARD" == "auto" || ! -e "$CARD" ]]; then
    local candidate
    for candidate in /dev/dri/card[0-9]*; do
      [[ -e "$candidate" ]] || continue
      if ./drm_fb_probe "$candidate" 2>/dev/null | awk '
        /connector .* connected/ { connected = 1 }
        /crtc [0-9]+: fb=[1-9][0-9]* .*size=/ { fb = 1 }
        END { exit !(connected && fb) }
      '; then
        CARD="$candidate"
        break
      fi
    done
  fi
  [[ "$CARD" != "auto" && -e "$CARD" ]] || return 1
  ./drm_fb_probe "$CARD" 2>/dev/null \
    | awk -v mode="$REQUIRED_MODE" '
      /connector .* connected/ { connected = 1 }
      /crtc [0-9]+: fb=[1-9][0-9]* .*size=/ {
        if (mode == "auto" || $0 ~ ("size=" mode)) {
          fb = 1
        }
      }
      END { exit !(connected && fb) }
    '
}

force_hdmi_mode() {
  if [[ "${HDMI_FORCE_MODE:-0}" == "0" || "$REQUIRED_MODE" == "auto" ]]; then
    return
  fi

  now="$(date +%s)"
  if (( now - last_force_run < FORCE_SECONDS )); then
    return
  fi
  last_force_run="$now"

  local auth_paths=()
  if [[ -n "${XAUTHORITY:-}" ]]; then
    auth_paths+=("$XAUTHORITY")
  fi
  while IFS=: read -r _ _ uid _ _ home _; do
    [[ "$uid" =~ ^[0-9]+$ ]] || continue
    (( uid >= 1000 )) || continue
    auth_paths+=("/run/user/$uid/gdm/Xauthority" "/run/user/$uid/Xauthority" "$home/.Xauthority")
  done < /etc/passwd

  local auth
  for auth in "${auth_paths[@]}"; do
    [[ -r "$auth" ]] || continue
    local output="$XRANDR_OUTPUT"
    if [[ "$output" == "auto" ]]; then
      output="$(DISPLAY="$DISPLAY_NAME" XAUTHORITY="$auth" xrandr --query 2>/dev/null | awk '/ connected/{print $1; exit}')"
      [[ -n "$output" ]] || continue
    fi
    if DISPLAY="$DISPLAY_NAME" XAUTHORITY="$auth" \
      xrandr --output "$output" --mode "$REQUIRED_MODE" --rate "$XRANDR_RATE" --primary \
        >/dev/null 2>&1; then
      return
    fi
    if DISPLAY="$DISPLAY_NAME" XAUTHORITY="$auth" \
      xrandr --output "$output" --mode "$REQUIRED_MODE" --primary \
        >/dev/null 2>&1; then
      return
    fi
  done

  if (( now - last_force_log >= 10 )); then
    echo "unable to force $XRANDR_OUTPUT to $REQUIRED_MODE through xrandr" >&2
    last_force_log="$now"
  fi
}

wait_for_hdmi() {
  while ! has_active_hdmi_fb; do
    force_hdmi_mode
    now="$(date +%s)"
    if (( now - last_wait_log >= 10 )); then
      echo "waiting for active HDMI framebuffer ${REQUIRED_MODE} on $CARD" >&2
      last_wait_log="$now"
    fi
    sleep "$WAIT_SECONDS"
  done
}

echo "display stream wrapper started card=$CARD required_mode=$REQUIRED_MODE xrandr_output=$XRANDR_OUTPUT" >&2

while true; do
  wait_for_hdmi
  force_hdmi_mode
  echo "active display framebuffer detected on $CARD; starting drm_rga_mpp_stream" >&2

  if [[ "$has_card_arg" -eq 1 ]]; then
    ./drm_rga_mpp_stream "${args[@]}" &
  else
    ./drm_rga_mpp_stream -c "$CARD" "${args[@]}" &
  fi
  child_pid="$!"
  while true; do
    wait "$child_pid"
    rc="$?"
    if kill -0 "$child_pid" 2>/dev/null; then
      continue
    fi
    break
  done
  child_pid=""

  echo "drm_rga_mpp_stream exited rc=$rc; waiting before retry" >&2
  if [[ "$rc" -eq 0 ]]; then
    echo "normal stream stop; exiting hotplug wrapper" >&2
    exit 0
  fi
  sleep "$RESTART_SECONDS"
done

