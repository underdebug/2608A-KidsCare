
#!/bin/bash
set -e

# Run from the script directory
cd "$(dirname "$0")"

# 1. Install application system-wide
sudo mkdir -p /usr/local/dimmer

sudo install -m 755 ./dimmer /usr/local/dimmer/dimmer
sudo install -m 644 ./dimmer.png /usr/local/dimmer/dimmer.png

# 2. Install application launcher system-wide
sudo tee /usr/share/applications/dimmer.desktop > /dev/null <<'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=Dimmer
Comment=Screen Dimmer
Exec=/usr/local/dimmer/dimmer
Icon=/usr/local/dimmer/dimmer.png
Terminal=false
StartupNotify=false
DBusActivatable=false
Categories=Utility;
EOF

# Set launcher permissions
sudo chmod 644 /usr/share/applications/dimmer.desktop

# 3. Add Dimmer to Ubuntu Dock
if command -v gsettings >/dev/null && \
   gsettings list-schemas | grep -qx "org.gnome.shell"; then

    python3 - <<'PY'
import ast
import subprocess

key = "org.gnome.shell"
setting = "favorite-apps"
desktop_file = "dimmer.desktop"

current = subprocess.check_output(
    ["gsettings", "get", key, setting],
    text=True
).strip()

favorites = [] if current == "@as []" else ast.literal_eval(current)

if desktop_file not in favorites:
    favorites.append(desktop_file)

    subprocess.run(
        ["gsettings", "set", key, setting, repr(favorites)],
        check=True
    )

print("Dimmer added to Ubuntu Dock!")
PY

else
    echo "GNOME Dock not detected."
    echo "Dimmer is installed in the application menu."
fi

echo "Dimmer installed successfully!"
