#!/usr/bin/env bash
# Installs the screenshot RECEIVER to /usr/local/screenshot-receiver and enables
# it as a systemd SYSTEM service (starts at boot, listens whether or not anyone
# is logged in). No display is needed, so a system service is the right fit.
#
# Run with sudo (ideally as the 'roots' user so /usr/screenshots is owned right):
#   sudo ./install-receiver.sh
set -euo pipefail

# ======== Hardcoded settings ========
INSTALL_DIR="/usr/local/screenshot-receiver"

RECEIVER_PORT="5000"
RECEIVER_DIR="/home/roots/Pictures/screenshots"
# ====================================

sudo systemctl stop screenshot-receiver.service
sudo systemctl disable screenshot-receiver.service

#sudo systemctl status screenshot-receiver.service
#journalctl -u screenshot-receiver -f

SRC="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo:  sudo $0" >&2
    exit 1
fi

# The normal user who invoked sudo, so saved files are owned by them.
RUN_USER="${SUDO_USER:-root}"

# 1. Build the binary if needed (build as the invoking user when possible).
if [ ! -x "$SRC/screenshot_receiver" ]; then
    echo ">> building screenshot_receiver ..."
    if [ "$RUN_USER" != "root" ]; then
        sudo -u "$RUN_USER" make -C "$SRC"
    else
        make -C "$SRC"
    fi
fi

# 2. Install program + wrapper + config, and create the shots directory.
mkdir -p "$INSTALL_DIR" "$RECEIVER_DIR"
cp "$SRC/screenshot_receiver" "$INSTALL_DIR/screenshot_receiver"
cp "$SRC/run-receiver.sh"     "$INSTALL_DIR/run-receiver.sh"
chmod +x "$INSTALL_DIR/screenshot_receiver" "$INSTALL_DIR/run-receiver.sh"
cat > "$INSTALL_DIR/config" <<EOF
RECEIVER_PORT=$RECEIVER_PORT
RECEIVER_DIR=$RECEIVER_DIR
EOF
chown -R "$RUN_USER":"$RUN_USER" "$INSTALL_DIR"
# The service runs as $RUN_USER, so it must own the save directory to write there.
chown "$RUN_USER":"$RUN_USER" "$RECEIVER_DIR"

# 3. Fill in the unit template (user) and install it.
sed "s/__USER__/$RUN_USER/g" "$SRC/screenshot-receiver.service" \
    > /etc/systemd/system/screenshot-receiver.service

# 4. Open the firewall RECEIVER_PORT if ufw is active.
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
    echo ">> opening RECEIVER_PORT $RECEIVER_PORT in ufw ..."
    ufw allow "$RECEIVER_PORT"/tcp || true
fi

# 5. Enable + (re)start.
systemctl daemon-reload
systemctl enable screenshot-receiver.service
systemctl restart screenshot-receiver.service

echo
echo ">> installed. listening on RECEIVER_PORT $RECEIVER_PORT."
echo ">> images will be saved under: $RECEIVER_DIR/YYYY-MM-DD/  (owned by $RUN_USER)"
echo ">> status:"
systemctl --no-pager status screenshot-receiver.service || true
echo
echo ">> live logs (Ctrl+C to stop; the service keeps running):"
echo "   journalctl -u screenshot-receiver -f"
