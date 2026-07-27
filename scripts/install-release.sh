#!/usr/bin/env bash
set -Eeuo pipefail

REPOSITORY="${REMYDESK_GITHUB_REPOSITORY:-LMXZ1258/RemyDesk}"
VERSION="latest"
INSTALLER_ARGS=()

usage() {
  cat <<'EOF'
Download, verify and install RemyDesk from GitHub Releases.

Usage:
  curl -fsSL https://raw.githubusercontent.com/LMXZ1258/RemyDesk/main/scripts/install-release.sh | \
    sudo bash -s -- [options] [-- installer-options]

Options:
  --repo OWNER/REPO   GitHub repository (default: LMXZ1258/RemyDesk)
  --version VERSION   Release version such as 0.2.0 (default: latest)
  -h, --help          Show help

Arguments after -- are forwarded to the embedded installer.
EOF
}

while (( $# )); do
  case "$1" in
    --repo) shift; REPOSITORY="${1:?--repo requires OWNER/REPO}" ;;
    --version) shift; VERSION="${1:?--version requires a version}" ;;
    --) shift; INSTALLER_ARGS=("$@"); break ;;
    -h|--help) usage; exit 0 ;;
    *) INSTALLER_ARGS+=("$1") ;;
  esac
  shift
done

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is required" >&2; exit 1; }

if [[ "$VERSION" == "latest" ]]; then
  BASE="https://github.com/$REPOSITORY/releases/latest/download"
else
  VERSION="${VERSION#v}"
  BASE="https://github.com/$REPOSITORY/releases/download/v$VERSION"
fi

WORKDIR="$(mktemp -d /tmp/remydesk-release.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"

curl -fL --retry 3 -o RemyDesk-RK3588-installer.sh "$BASE/RemyDesk-RK3588-installer.sh"
curl -fL --retry 3 -o SHA256SUMS "$BASE/SHA256SUMS"
grep '  RemyDesk-RK3588-installer.sh$' SHA256SUMS | sha256sum -c -
chmod 0755 RemyDesk-RK3588-installer.sh
exec bash ./RemyDesk-RK3588-installer.sh "${INSTALLER_ARGS[@]}"

