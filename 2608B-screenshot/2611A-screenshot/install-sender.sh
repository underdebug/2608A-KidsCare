#!/usr/bin/env bash
# Installs the screenshot SENDER entirely under /usr/local/screenshot-sender and
# enables it as a systemd SYSTEM service. Screenshots go to SENDER_DIR.
#
# The sender captures the screen, so the service runs AS your desktop user and
# the wrapper points it at your X session (DISPLAY + XAUTHORITY). You must be
# logged in graphically for capture to succeed.
#
# Run with sudo, from the account that owns the desktop session:
#   sudo ./install-sender.sh
set -euo pipefail

# ======== Hardcoded settings (edit if needed) ========
INSTALL_DIR="/usr/local/screenshot-sender"

RECEIVER_IP="mingjie.local"   # hostname or IP of the receiver
RECEIVER_PORT="5000"
RECEIVER_INTERVAL="300"         # seconds  (300 = 5 minutes) — how often to transmit to the receiver
SENDER_DIR="/home/roots/Pictures/screenshots"  # local capture directory
SENDER_INTERVAL="120"          # seconds  (60 = 1 minute) — how often to capture+save locally

# =====================================================

SRC="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo:  sudo $0" >&2
    exit 1
fi

# The normal desktop user who invoked sudo -- the service runs as them.
RUN_USER="${SUDO_USER:-root}"
RUN_GROUP="$(id -gn "$RUN_USER")"

# 1. Build the sender when its source has changed, before replacing the service.
echo ">> building screenshot_sender ..."
if [ "$RUN_USER" != "root" ]; then
    sudo -u "$RUN_USER" make -C "$SRC" screenshot_sender
else
    make -C "$SRC" screenshot_sender
fi

# 2. Install program + wrapper + config under /usr/local, and create the
#    local save directory owned by the desktop user.
mkdir -p "$INSTALL_DIR" "$SENDER_DIR"
# Install to a new inode, then replace the pathname atomically. Linux does not
# allow overwriting an executable while an existing service is running it.
TEMP_BIN="$(mktemp "$INSTALL_DIR/.screenshot_sender.XXXXXX")"
trap 'rm -f "$TEMP_BIN"' EXIT
install -m 755 "$SRC/screenshot_sender" "$TEMP_BIN"
mv -f "$TEMP_BIN" "$INSTALL_DIR/screenshot_sender"
trap - EXIT
cp "$SRC/run-sender.sh"     "$INSTALL_DIR/run-sender.sh"
chmod +x "$INSTALL_DIR/screenshot_sender" "$INSTALL_DIR/run-sender.sh"
{
    printf 'RECEIVER_IP=%q\n' "$RECEIVER_IP"
    printf 'RECEIVER_PORT=%q\n' "$RECEIVER_PORT"
    printf 'RECEIVER_INTERVAL=%q\n' "$RECEIVER_INTERVAL"
    printf 'SENDER_DIR=%q\n' "$SENDER_DIR"
    printf 'SENDER_INTERVAL=%q\n' "$SENDER_INTERVAL"
} > "$INSTALL_DIR/config"
chown root:root "$INSTALL_DIR" "$INSTALL_DIR/screenshot_sender" "$INSTALL_DIR/run-sender.sh" "$INSTALL_DIR/config"
chmod 755 "$INSTALL_DIR"
chmod 644 "$INSTALL_DIR/config"
chown "$RUN_USER":"$RUN_GROUP" "$SENDER_DIR"

# 3. Install the SYSTEM service unit, running as your user.
sed -e "s/^User=__USER__$/User=$RUN_USER/" \
    -e "s/^Group=__USER__$/Group=$RUN_GROUP/" "$SRC/screenshot-sender.service" \
    > /etc/systemd/system/screenshot-sender.service

# 4. Enable + (re)start.
systemctl daemon-reload
systemctl enable screenshot-sender.service
systemctl restart screenshot-sender.service

echo
echo ">> installed as a SYSTEM service running as user '$RUN_USER'."
echo ">> program files live in $INSTALL_DIR ; unit in /etc/systemd/system/."
echo ">> saving screenshots to $SENDER_DIR every ${SENDER_INTERVAL}s."
echo ">> sending to $RECEIVER_IP:$RECEIVER_PORT every ${RECEIVER_INTERVAL}s."
echo ">> status:"
systemctl --no-pager status screenshot-sender.service || true
echo
echo ">> live logs:  journalctl -u screenshot-sender -f"
