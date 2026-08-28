#!/bin/bash
sudo mkdir -p /usr/local/dimmer
sudo cp dimmer dimmer.png /usr/local/dimmer/
sudo chmod +x /usr/local/dimmer/dimmer

sudo tee dimmer.desktop > /dev/null <<EOF
[Desktop Entry]
Type=Application
Name=Dimmer
Exec=/usr/local/dimmer/dimmer
Icon=/usr/local/dimmer/dimmer.png
Terminal=false
EOF

cp ./dimmer.desktop ~/Desktop/
chmod +x ~/Desktop/dimmer.desktop
gio set ~/Desktop/dimmer.desktop metadata::trusted true

echo "Done."

