#!/usr/bin/env bash
# Run the Orca CLI against an Orca runtime that only exists on the tailnet.
#
# Why this exists: `orca` (Resources/bin/orca) launches the CLI as
# ELECTRON_RUN_AS_NODE and deliberately unsets NODE_OPTIONS. The CGNAT preload
# that makes the desktop app reach 100.64.0.0/10 is prepended to the app's main
# process only, so a plain CLI process has no route to the tailnet address and
# `--environment <tailnet runtime>` fails with:
#
#   remote_runtime_unavailable ... the host did not answer
#
# even while `curl --socks5-hostname 127.0.0.1:1055 http://<cgnat>:6768/` works.
# This wrapper loads the same preload into the CLI process through NODE_OPTIONS,
# so CGNAT connects are rewritten to 127.0.0.1:${ORCA_NETD_FORWARD_PORT}.
#
# Usage:
#   projects/orca-netd/scripts/orca-cli.sh repo list --environment cc-orca
#   projects/orca-netd/scripts/orca-cli.sh worktree list --environment cc-orca
#
# Anything reachable on the public internet or loopback also works, so this is a
# drop-in replacement for `orca` while the forwarder is up.
set -euo pipefail

APP="${ORCA_APP:-/Applications/Orca.app}"
PRELOAD="${ORCA_NETD_PRELOAD:-$HOME/.local/share/orca-netd/socks-cgnat-preload.js}"
CLI="$APP/Contents/Resources/app.asar.unpacked/out/cli/index.js"

for path in "$APP/Contents/MacOS/Orca" "$PRELOAD" "$CLI"; do
  if [ ! -e "$path" ]; then
    echo "orca-cli: missing $path" >&2
    exit 1
  fi
done

# The preload drops NODE_OPTIONS from the environment as soon as it loads, so
# spawned children (git, shells, agents) do not inherit the --require.
ELECTRON_RUN_AS_NODE=1 \
NODE_OPTIONS="--require $PRELOAD" \
  exec "$APP/Contents/MacOS/Orca" "$CLI" "$@"
