# g15-kbd-backlight

Tiny (~23 KB binary, ~450 KB RAM) C daemon and CLI tool that makes the **Dell G15 5530** keyboard backlight and **`Fn+F5`** function key actually work on Linux (tested on Arch Linux + KDE Plasma 6 Wayland).

> ⚠️ **Disclaimer:** This entire repository was **vibe coded to hell and back** with an AI agent at midnight because the keyboard wouldn't light up. Expect raw USB HID byte arrays, reverse-engineered Alienware AW-ELC opcodes, and zero enterprise abstractions. It works, it's fast, and it uses almost no resources—use at your own risk.

---

## Why is this needed on the Dell G15 5530?

If you install Linux on a Dell G15 5530, three separate things conspire to break the keyboard backlight:

1. **Fake SMBIOS LED interface:** The kernel exposes `/sys/class/leds/dell::kbd_backlight`, so KDE/GNOME/UPower think they are controlling a standard 3-level Dell backlight (`0`, `1`, `2`). The KDE OSD pops up and the brightness slider moves, but the physical keyboard stays pitch black because the LEDs are actually wired to an internal **Alienware AW-ELC USB HID controller (`187c:0551`)**.
2. **Stuck `Fn+F5` key in upstream `udev`:** In `60-keyboard.hwdb`, Dell's `0x69` scancode (`Fn+F5`) is mapped without the `!` (`force-release`) flag on the G15 5530. Pressing `Fn+F5` once leaves the key permanently stuck in the "pressed" state in `atkbd` until reboot.
3. **Ghost animations & wake lag on `187c:0551`:**
   - The AW-ELC controller ships with factory animation slots (`0x0081`) and AC/DC power state slots (`0x005B..0x0060`) in flash that override the default user slot (`0x0061`) unless explicitly purged.
   - Recording static colors with slow action durations (`duration=2000, tempo=250`) causes a noticeable delay when waking the backlight from `0%` (`dim=100`) to `50%`. Programming the slots with `duration=1, tempo=2` and keeping the `libusb` handle warm makes `0% -> 50% -> 100%` transitions instant.

---

## Features

- **`Fn+F5` cycling (`0% -> 50% -> 100% -> 0%`)** with instant response and KDE Plasma OSD notification.
- **Bidirectional KDE / UPower sync:** Watches `/sys/class/leds/dell::kbd_backlight/brightness` via `inotify`, so dragging the **KDE System Tray Keyboard Brightness slider** or letting KDE PowerDevil dim the keyboard on idle also dims the physical USB keyboard.
- **Zero-wear startup:** Flashes the AW-ELC animation slots once on first run (or when changing RGB color) and only sends the 9-byte dim packet (`0x26`) on subsequent boots and brightness toggles.
- **4-Zone RGB color control** via CLI (persisted across reboots in `~/.config/g15-kbd-backlight.conf`).

---

## Dependencies

- `gcc` & `pkg-config`
- `libusb` (`libusb-1.0`)
- `libevdev`

On Arch Linux:
```bash
sudo pacman -S gcc pkgconf libusb libevdev
```

---

## Installation

```bash
git clone https://github.com/De-k-i/g15-kbd-backlight.git ~/.local/share/g15-kbd-backlight
cd ~/.local/share/g15-kbd-backlight
./install.sh
```

`install.sh` is idempotent and will:
1. Compile `g15-kbd-backlight.c` to `~/.local/bin/g15-kbd-backlight`.
2. Install `/etc/udev/rules.d/70-dell-g15-awelc.rules` (`uaccess` for USB `187c:0551`).
3. Install `/etc/udev/hwdb.d/90-dell-g15-kbd.hwdb` (`KEYBOARD_KEY_69=!kbdillumtoggle` so `Fn+F5` forces key release).
4. Enable and start the `g15-kbd-backlight.service` systemd user service.

---

## CLI Usage

```bash
g15-kbd-backlight                    # Show current brightness & RGB color
g15-kbd-backlight cycle              # Cycle 0% -> 50% -> 100% -> 0%
g15-kbd-backlight brightness 100     # Set brightness (0-100)
g15-kbd-backlight color ffffff       # Set static RGB color (Pure White)
g15-kbd-backlight color 00ffff       # Set static RGB color (Alienware Cyan)
g15-kbd-backlight color ff5500       # Set static RGB color (Orange)
```

---

## Credits

Protocol knowledge adapted from [tr1xem/AWCC](https://github.com/tr1xem/AWCC) and [tr1xem/alienfx-linux](https://github.com/tr1xem/alienfx-linux).
