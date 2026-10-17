# g15-kbd-backlight

Tiny (~23 KB binary, ~450 KB RAM) C daemon and CLI tool that makes the **Dell G15 5530** keyboard backlight and **`Fn+F5`** function key work on Linux (tested on Arch Linux + KDE Plasma 6 Wayland).

> ⚠️ **Disclaimer:** This repository was **vibe coded to hell and back** with an AI agent at midnight because the keyboard wouldn't light up. Expect raw USB HID byte arrays, Alienware AW-ELC opcodes, and zero abstractions.

---

## Verified Hardware Quirks on the Dell G15 5530

Everything below was directly inspected and verified on a Dell G15 5530 (`Board 0KC8TC`):

1. **Dummy SMBIOS LED interface (`/sys/class/leds/dell::kbd_backlight`):**
   - The `dell-laptop` kernel driver exposes `/sys/class/leds/dell::kbd_backlight` (`max_brightness = 2`), which UPower and KDE PowerDevil read and write.
   - Changing `/sys/class/leds/dell::kbd_backlight/brightness` moves the desktop brightness slider and triggers the KDE OSD, but does not affect the physical LEDs because the keyboard backlight is actually controlled by an internal **Alienware AW-ELC USB HID controller (`187c:0551`, firmware `2.1.8`, platform `0e 0a` with `4` RGB zones: `0x00..0x03`)**.

2. **Missing `force-release` quirk for `Fn+F5` in upstream `udev`:**
   - Pressing `Fn+F5` on the internal `AT Translated Set 2 keyboard` (`i8042`) emits make scancode `0x69` (`105`) with no hardware break scancode on release.
   - Upstream `60-keyboard.hwdb` defines `KEYBOARD_KEY_68=!performance` (scancode `104`, the G-Mode key next to `Fn+F5`), which adds `104` to `/sys/devices/platform/i8042/serio0/force_release`, but omits `KEYBOARD_KEY_69` (`105`).
   - Without a `force-release` rule (`!`) on `0x69`, the kernel `atkbd` driver maps scancode `105` to `KEY_F18` and leaves it permanently stuck in the pressed state (`value = 1`) after the first press.
   - Adding `KEYBOARD_KEY_69=!kbdillumtoggle` via `/etc/udev/hwdb.d/90-dell-g15-kbd.hwdb` adds `105` to `/sys/devices/platform/i8042/serio0/force_release` (`104-105,369-370`) so the kernel synthesizes key release properly.

3. **Controlling the `187c:0551` AW-ELC USB controller:**
   - Brightness is controlled via 33-byte HID `SET_REPORT` (`bmRequestType = 0x21`, `bRequest = 0x09`, `wValue = 0x0202`) command `03 26 <dim> 00 04 00 01 02 03`, where `<dim>` is `0..100` (`0` = 100% brightness, `100` = 0% / off).
   - Static RGB colors are programmed by recording `CMD_ZONE_SELECT (0x23)` + `CMD_ADD_ACTION (0x24)` packets across the 4 zones (`0x00..0x03`) into the controller's animation slots (`0x0061` default and `0x005C`/`0x005D`/`0x005F` power-state slots).

---

## What This Daemon Does

- **Listens for `Fn+F5` (`MSC_SCAN = 105`)** on `/dev/input/by-path/platform-i8042-serio-0-event-kbd` (read-only, non-grabbing) and cycles keyboard brightness `0% -> 50% -> 100% -> 0%`.
- **Keeps a persistent `libusb` device handle** in daemon mode (releasing USB interface `0` back to `usbhid` between transfers) so brightness changes require only a single USB control transfer.
- **Bidirectional KDE / UPower sync:** Watches `/sys/class/leds/dell::kbd_backlight/brightness` with `inotify` so changes from the **KDE System Tray Keyboard Brightness slider** or KDE PowerDevil idle dimming also update the physical USB keyboard backlight.
- **CLI control** for setting custom brightness (`0-100`) or 24-bit RGB hex colors, persisted in `~/.config/g15-kbd-backlight.conf`.

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
3. Install `/etc/udev/hwdb.d/90-dell-g15-kbd.hwdb` (`KEYBOARD_KEY_69=!kbdillumtoggle`).
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

AW-ELC USB HID protocol reference adapted from [tr1xem/AWCC](https://github.com/tr1xem/AWCC) and [tr1xem/alienfx-linux](https://github.com/tr1xem/alienfx-linux).
