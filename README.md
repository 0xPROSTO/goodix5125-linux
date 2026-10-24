# goodix5125 — Linux driver for the Goodix 27c6:5125 fingerprint sensor

Huawei MateBook D (NbD-WDH9 / NBD-WXX9), sensor in the power button.
A libfprint driver, so it works with fprintd, GDM / GNOME lock screen and sudo.

> **Experimental.** Written from scratch for one laptop and tested only with
> one person's fingers. Rejection of other fingers looks solid on that data
> (0 of ~200 impostor tries), but the false-accept rate is not established
> the way commercial matchers are, and there is no anti-spoofing. Use it for
> convenience, with your password as the real lock, not as the only factor
> protecting anything that matters.

Check `lsusb` for `27c6:5125` first. Other Goodix sensors need other drivers.

## Install (CachyOS / Arch)

1. The sensor's TLS key must be in `psk.bin` (32 bytes) next to this file.
   It is the key the Windows driver provisioned; see "The key" below.
2. `./install.sh` (or `./install.sh --sudo` to also accept the finger for sudo).
   It builds libfprint 1.94.100 + this driver as `libfprint-goodix5125`
   (replaces the distro `libfprint`), installs fprintd, and puts the key in
   `/etc/goodix5125/psk.bin` (root, 0600).
3. Enroll: GNOME Settings → System → Users → Fingerprint Login, or
   `fprintd-enroll -f right-index-finger` (15 touches).

Tips: an index finger works best — the sensor sees only ~4×5 mm, and a thumb
lands on a different patch each time. Touch naturally; small changes of
angle (up to ±60°) and position are handled.

To remove: `sudo pacman -Rns libfprint-goodix5125 fprintd`, restore
`/etc/pam.d/sudo.before-fprintd` if you used `--sudo`, delete `/etc/goodix5125`.

## How it works

| layer | file |
|---|---|
| framing (outer pack + inner message, checksums) | `lib/proto.c` |
| command/ACK layer, transport abstraction | `lib/device.c`, `lib/usb.c` |
| TLS 1.2 PSK (sensor = client, host = server, memory BIOs) | `lib/tls.c` |
| init (OTP calibration, MCU config), FDT finger detection, capture | `lib/sensor.c` |
| 12-bit decode, background subtraction, equalisation, quality | `lib/image.c` |
| matcher: corners + patches, angle search, RANSAC, aligned-overlap check | `lib/match.c` |
| libfprint `FpDevice` driver (worker thread) | `driver/goodix5125.c` |

Measured on this sensor: no impostor accepted in 192 offline tries (best
score 3, threshold 4) nor in 10 live ones; the enrolled index finger passes
about 8 of 10 touches. A touch is answered in ~0.3 s; the sensor needs
~0.8 s to get ready when an action starts.

## The key

Images travel over TLS with a pre-shared key that the Windows Goodix driver
generates and writes to the sensor. Without it nothing works. `vm/` holds the
throwaway-VM tooling used once to let the official driver provision the
sensor and unseal the key from `C:\ProgramData\Goodix\Goodix_Cache.bin`
(`tools/extract-psk.ps1`). Re-provisioning by Windows (e.g. a later Windows
install with Windows Hello) changes the key; extract it again then.

## Development

```
make && make check                 # library, tools, hardware-free tests
build/gxctl info|tls|bg|capture N  # bring-up tool (needs the udev rule)
driver/apply.sh path/to/libfprint  # add the driver to a libfprint tree
```

See AGENTS.md for the architecture, invariants and test tools.

Protocol knowledge builds on goodix-fp-dump and goodix-5117-driver (LGPL-2.1+).

## License

LGPL-2.1-or-later, like libfprint (see `LICENSE`). Never publish your
`psk.bin` or fingerprint captures: both are git-ignored.
