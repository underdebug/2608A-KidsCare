#!/usr/bin/env bash
# Wrapper launched by the systemd SYSTEM service (runs as your desktop user).
# Resolves the graphical session environment, loads config, then launches the
# screenshot sender. X11 capture needs DISPLAY + XAUTHORITY; Wayland uses the
# session bus and ScreenCast portal instead.
set -euo pipefail

# --- Load config written by install.sh ------------------------------------
CONF="/usr/local/screenshot-sender/config"
if [ -r "$CONF" ]; then
    # shellcheck source=/dev/null
    . "$CONF"
fi

# --- Settings (config file wins; these are only fallbacks) -----------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-$SCRIPT_DIR/screenshot_sender}"
RECEIVER_IP="${RECEIVER_IP:-mingjie.local}"
RECEIVER_PORT="${RECEIVER_PORT:-5000}"
RECEIVER_INTERVAL="${RECEIVER_INTERVAL:-300}"
SENDER_DIR="${SENDER_DIR:-/home/roots/Pictures/screenshots}"
SENDER_INTERVAL="${SENDER_INTERVAL:-120}"

# --- X display -------------------------------------------------------------
# Adjust if your session is not on :0 (check with `echo $DISPLAY` in a terminal).
export DISPLAY="${DISPLAY:-:0}"

uid="$(id -u)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$uid}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
# A system service does not inherit the graphical session environment.
if [ -z "${XDG_SESSION_TYPE:-}" ] && compgen -G "$XDG_RUNTIME_DIR/wayland-*" >/dev/null; then
    export XDG_SESSION_TYPE=wayland
fi
if [ "${XDG_SESSION_TYPE:-}" != "wayland" ]; then
    export DISPLAY="${DISPLAY:-:0}"

    # Find the cookie each time the service starts, including GNOME's
    # dynamically named Xwayland cookie. Prefer an explicit XAUTHORITY.
    find_xauthority() {
        if [ -n "${XAUTHORITY:-}" ] && [ -r "$XAUTHORITY" ]; then
            return 0
        fi
        local cand
        for cand in "/run/user/$uid"/.mutter-Xwaylandauth.* \
                    "/run/user/$uid/gdm/Xauthority" "$HOME/.Xauthority"; do
            if [ -f "$cand" ] && [ -r "$cand" ]; then
                export XAUTHORITY="$cand"
                return 0
            fi
        done
        return 1
    }

    # The graphical session may start after the system service.
    for _ in $(seq 1 60); do
        if find_xauthority; then break; fi
        sleep 5
    done
    if ! find_xauthority; then
        echo "screenshot-sender: no readable Xauthority file found for user $(id -un)" >&2
    fi
fi

echo "screenshot-sender: session=${XDG_SESSION_TYPE:-unknown} DISPLAY=${DISPLAY:-<unset>} XAUTHORITY=${XAUTHORITY:-<unset>}"
echo "screenshot-sender: save=$SENDER_DIR every ${SENDER_INTERVAL}s; send to $RECEIVER_IP:$RECEIVER_PORT every ${RECEIVER_INTERVAL}s"

exec "$BIN" "$RECEIVER_IP" "$RECEIVER_PORT" "$SENDER_DIR" "$SENDER_INTERVAL" "$RECEIVER_INTERVAL"
