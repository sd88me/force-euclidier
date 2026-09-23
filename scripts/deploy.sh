#!/usr/bin/env bash
# Deploy Euclidier onto a live MockbaMod Force/MPC in one command.
# Usage: scripts/deploy.sh user@force-ip
#
# Unlike this project's sibling addons (force-maze, force-dx7, force-jv880,
# force-acid), Euclidier has no manage.sh ENABLE/DISABLE contract - it's a
# single AUTOLAUNCHABLE binary that nodeServer's own Modules page starts and
# stops directly, so this script only needs to copy the files into place.
set -euo pipefail
cd "$(dirname "$0")/.."

APP_DIR="Euclidier"
BIN="bin/euclidier"

HOST="${1:?usage: scripts/deploy.sh user@force-ip}"

if [ ! -f "$BIN" ]; then
  echo "$BIN not found - run build/build.sh first." >&2
  exit 1
fi

mmPath="$(ssh "$HOST" 'cat /dev/shm/.mmPath')"
echo "== remote mmPath: $mmPath =="

installroot="$mmPath/AddOns/$APP_DIR"
ssh "$HOST" "mkdir -p '$installroot'"
scp "$BIN" "$HOST:$installroot/euclidier"
scp addon/NSMODULE.json "$HOST:$installroot/NSMODULE.json"
scp addon/shadow_page.conf "$HOST:$installroot/shadow_page.conf"
ssh "$HOST" "chmod +x '$installroot/euclidier'"

cat <<EOF
== Euclidier deployed to $installroot ==

Start/stop it from the nodeServer Modules page (/moduler) - it's
AUTOLAUNCHABLE, so it can also be left running at boot from there.

For the touchscreen GUI: install force-shadow separately (see its own
repo), then open the page with SHIFT+SCENE-7.
EOF
