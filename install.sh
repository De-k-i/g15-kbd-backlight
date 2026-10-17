#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"

gcc -O2 -Wall -Wextra -s "$SCRIPT_DIR/g15-kbd-backlight.c" \
    -o "$HOME/.local/bin/g15-kbd-backlight" \
    $(pkg-config --cflags --libs libusb-1.0 libevdev)

cat << 'EOF' > "$HOME/.config/systemd/user/g15-kbd-backlight.service"
[Unit]
Description=Dell G15 5530 Keyboard Backlight & Fn+F5 Hotkey Daemon
After=graphical-session.target

[Service]
Type=simple
ExecStart=%h/.local/bin/g15-kbd-backlight --daemon
Restart=always
RestartSec=2

[Install]
WantedBy=default.target
EOF

sudo sh -c '
cat << "EOF" > /etc/udev/rules.d/70-dell-g15-awelc.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="187c", ATTRS{idProduct}=="0551", MODE="0666", TAG+="uaccess"
EOF

cat << "EOF" > /etc/udev/hwdb.d/90-dell-g15-kbd.hwdb
evdev:atkbd:dmi:bvn*:bvr*:bd*:svnDell*:pn*G15*5530*:*
 KEYBOARD_KEY_69=!kbdillumtoggle
EOF

systemd-hwdb update
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=187c
udevadm trigger --subsystem-match=input
'

systemctl --user daemon-reload
systemctl --user enable --now g15-kbd-backlight.service
systemctl --user restart g15-kbd-backlight.service
