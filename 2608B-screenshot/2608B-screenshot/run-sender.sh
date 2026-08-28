#!/usr/bin/env bash
# Wrapper launched by the systemd SYSTEM service (runs as your desktop user).
# Resolves the X11 session environment (DISPLAY + XAUTHORITY), loads config,
# then launches the screenshot sender. A system service may start before you
# log in graphically, so we first WAIT for your X session to appear.
set -euo pipefail

# --- Load config written by install.sh ------------------------------------
CONF="/usr/local/screenshot-sender/config"
if [ -r "$CONF" ]; then
    # shellcheck source=/dev/null
    . "$CONF"
fi

# --- Settings (config file wins; these are only fallbacks) -----------------
BIN="${BIN:-/usr/local/screenshot-sender/screenshot_sender}"
RECEIVER_IP="${RECEIVER_IP:-mingjie.local}"
RECEIVER_PORT="${RECEIVER_PORT:-5000}"
RECEIVER_INTERVAL="${RECEIVER_INTERVAL:-300}"
SENDER_DIR="${SENDER_DIR:-/home/roots/Pictures/screenshots}"
SENDER_INTERVAL="${SENDER_INTERVAL:-120}"

# --- X display -------------------------------------------------------------
# Adjust if your session is not on :0 (check with `echo $DISPLAY` in a terminal).
export DISPLAY="${DISPLAY:-:0}"

uid="$(id -u)"

# --- Wait for a live X session --------------------------------------------
# On a system service, the X cookie may not exist until someone logs in.
# Wait up to ~5 minutes for it, then continue (the sender also retries).
for _ in $(seq 1 60); do
    if [ -n "${XAUTHORITY:-}" ] && [ -r "${XAUTHORITY}" ]; then break; fi
    if [ -r "$HOME/.Xauthority" ] || [ -r "/run/user/$uid/gdm/Xauthority" ]; then break; fi
    sleep 5
done

# --- X authority cookie ----------------------------------------------------
if [ -z "${XAUTHORITY:-}" ] || [ ! -r "${XAUTHORITY:-/nonexistent}" ]; then
    for cand in \
        "$HOME/.Xauthority" \
        "/run/user/$uid/gdm/Xauthority"; do
        if [ -r "$cand" ]; then
            export XAUTHORITY="$cand"
            break
        fi
    done
fi

echo "screenshot-sender: DISPLAY=$DISPLAY XAUTHORITY=${XAUTHORITY:-<unset>}"
echo "screenshot-sender: save=$SENDER_DIR every ${SENDER_INTERVAL}s; send to $RECEIVER_IP:$RECEIVER_PORT every ${RECEIVER_INTERVAL}s"

exec "$BIN" "$RECEIVER_IP" "$RECEIVER_PORT" "$RECEIVER_INTERVAL" "$SENDER_DIR" "$SENDER_INTERVAL"