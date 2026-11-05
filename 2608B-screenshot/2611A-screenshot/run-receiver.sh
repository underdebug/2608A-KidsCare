#!/usr/bin/env bash
# Wrapper for the systemd USER service (receiver side).
# Just listens on a TCP port and writes incoming PNGs to disk — no X11
# display is needed here (that's only required on the sender side).
set -euo pipefail

# --- Load config written by install-receiver.sh (if any) -------------------
CONF="/usr/local/screenshot-receiver/config"
if [ -r "$CONF" ]; then
    # shellcheck source=/dev/null
    . "$CONF"
fi

# --- Settings (config file wins; these are only fallbacks) -----------------
BIN="${BIN:-/usr/local/screenshot-receiver/screenshot_receiver}"
RECEIVER_PORT="${RECEIVER_PORT:-5000}"
RECEIVER_DIR="${RECEIVER_DIR:-/home/roots/Pictures/screenshots}"

echo "screenshot-receiver: listening on port $RECEIVER_PORT, saving under $RECEIVER_DIR"

exec "$BIN" "$RECEIVER_PORT" "$RECEIVER_DIR"
