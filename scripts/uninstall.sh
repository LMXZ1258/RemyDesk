#!/usr/bin/env bash
set -euo pipefail

if [[ "$EUID" -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

systemctl disable --now remydesk.service remydesk-desktop.service remydesk-display-mode.service 2>/dev/null || true
rm -f /etc/systemd/system/remydesk.service /etc/systemd/system/remydesk-desktop.service /etc/systemd/system/remydesk-display-mode.service
rm -f /etc/tmpfiles.d/remydesk.conf /etc/udev/rules.d/70-remydesk.rules /etc/sudoers.d/remydesk
rm -rf /opt/remydesk
systemctl daemon-reload
udevadm control --reload-rules

echo "RemyDesk binaries and services removed."
echo "Configuration and user data remain in /etc/remydesk, /var/lib/remydesk and /srv/remydesk."
