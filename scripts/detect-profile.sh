#!/usr/bin/env bash
set -euo pipefail

MODEL="${REMYDESK_BOARD_MODEL:-}"
COMPATIBLE="${REMYDESK_BOARD_COMPATIBLE:-}"

if [[ -z "$MODEL" && -r /proc/device-tree/model ]]; then
  MODEL="$(tr -d '\0' </proc/device-tree/model)"
fi
if [[ -z "$COMPATIBLE" && -r /proc/device-tree/compatible ]]; then
  COMPATIBLE="$(tr '\0' ',' </proc/device-tree/compatible)"
fi

IDENTITY="${MODEL},${COMPATIBLE}"
IDENTITY="${IDENTITY,,}"

case "$IDENTITY" in
  *orange*pi*5*plus*) PROFILE=orangepi-5-plus ;;
  *aio-3399j*|*firefly*rk3399*) PROFILE=firefly-rk3399 ;;
  *firefly*rk3588*|*roc-rk3588*|*itx-3588*) PROFILE=firefly-rk3588 ;;
  *) PROFILE=generic-rk3588 ;;
esac

if [[ "${1:-}" == "--explain" ]]; then
  printf 'profile=%s\nmodel=%s\ncompatible=%s\n' "$PROFILE" "${MODEL:-unknown}" "${COMPATIBLE:-unknown}"
else
  printf '%s\n' "$PROFILE"
fi
