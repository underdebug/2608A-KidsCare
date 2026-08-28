#!/usr/bin/env bash
# Installs the screenshot SENDER entirely under /usr/local/screenshot-sender and
# enables it as a systemd SYSTEM service. Nothing is written under /home.
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
SENDER_INTERVAL="120"          # seconds  (120 = 2 minute) — how often to capture+save locally

# =====================================================

sudo systemctl stop screenshot-sender.service
sudo systemctl disable screenshot-sender.service

SRC="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo:  sudo $0" >&2
    exit 1
fi

# The normal desktop user who invoked sudo -- the service runs as them.
RUN_USER="${SUDO_USER:-root}"

# 1. Build the binary if needed (as the invoking user when possible).
if [ ! -x "$SRC/screenshot_sender" ]; then
    echo ">> building screenshot_sender ..."
    if [ "$RUN_USER" != "root" ]; then
        sudo -u "$RUN_USER" make -C "$SRC"
    else
        make -C "$SRC"
    fi
fi

# 2. Install program + wrapper + config under /usr/local, and create the
#    local save directory owned by the desktop user.
mkdir -p "$INSTALL_DIR" "$SENDER_DIR"
cp "$SRC/screenshot_sender" "$INSTALL_DIR/screenshot_sender"
cp "$SRC/run-sender.sh"     "$INSTALL_DIR/run-sender.sh"
chmod +x "$INSTALL_DIR/screenshot_sender" "$INSTALL_DIR/run-sender.sh"
cat > "$INSTALL_DIR/config" <<EOF
RECEIVER_IP=$RECEIVER_IP
RECEIVER_PORT=$RECEIVER_PORT
RECEIVER_INTERVAL=$RECEIVER_INTERVAL
SENDER_DIR=$SENDER_DIR
SENDER_INTERVAL=$SENDER_INTERVAL
EOF
chown -R "$RUN_USER":"$RUN_USER" "$INSTALL_DIR"
chown "$RUN_USER":"$RUN_USER" "$SENDER_DIR"

# 3. Install the SYSTEM service unit, running as your user.
sed "s/__USER__/$RUN_USER/g" "$SRC/screenshot-sender.service" \
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
