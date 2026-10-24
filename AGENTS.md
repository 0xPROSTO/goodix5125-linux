# AGENTS.md: goodix5125 fingerprint driver

These notes are for coding agents (and humans) changing this project without
its history. Read them before editing. The "Invariants" section records
mistakes that already broke the real sensor once.

## What this is

A libfprint driver for the **Goodix 27c6:5125** USB fingerprint sensor in the
power button of a Huawei MateBook D (NbD-WDH9). It was developed on CachyOS
(Arch) with GNOME/GDM and fprintd 1.94. No upstream driver exists;
this one is written from scratch, building on the protocol knowledge of
goodix-fp-dump / goodix-5117-driver (LGPL-2.1+).

- Firmware `GF_ST411SEC_APP_12509`, chip id `a2042500`. Same family as 5110/5117.
- Image sensor with host-side matching: the laptop receives 88×80 12-bit
  images over TLS and compares them itself. The sensor does not match.
- It works with fprintd, the GDM/GNOME lock screen, and sudo (`pam_fprintd`).

## Layout

| path | what |
|---|---|
| `lib/gx5125.h` | the whole library API, constants, and tuned thresholds |
| `lib/proto.c` | outer "pack" (flags, len, checksum) + inner "message" (cmd, len, payload, checksum) |
| `lib/device.c` | command layer: send pack, ACK + reply, `gx_drain`, receive loop |
| `lib/usb.c` | libusb transport, used only by the CLI tools |
| `lib/tls.c` | TLS 1.2 PSK over memory BIOs. **Sensor = client, host = server** |
| `lib/sensor.c` | init (OTP, calibration, MCU config), FDT finger detection, capture, `gx_sensor_unstick` |
| `lib/image.c` | decode (6 bytes = 4 px), crop 88×80 → 64×80 (columns 64-87 are dead), background subtraction, equalisation, coverage/empty checks |
| `lib/match.c` | matcher (see below) |
| `driver/goodix5125.c` | the libfprint `FpDevice` driver: worker thread, cancellation, fprintd integration |
| `driver/apply.sh` | copies `lib/*` + driver into a libfprint tree and registers it in meson |
| `packaging/PKGBUILD` | builds `libfprint-goodix5125` (upstream libfprint 1.94.100 + this driver; replaces the distro `libfprint`) |
| `install.sh` | owner-run installer: build + `pacman -U` + fprintd + key to `/etc/goodix5125/psk.bin` (+ `--sudo` PAM) |
| `tests/test.c` | 24 hardware-free checks (`make check`) |
| `tools/` | bring-up, evaluation and live-test tools (below) |
| `vm/`, `tools/extract-psk.ps1`, `tools/probe.py` | one-time key provisioning via a throwaway Windows VM. Already done, the VM is destroyed |

## Build and test

```sh
make && make check                  # lib + gxctl/matcheval/seteval + 24 tests, no hardware
driver/apply.sh build/libfprint && ninja -C build/libfprint/_build   # compile the driver in-tree
cd packaging && makepkg -f          # the package the owner installs
tools/evalset.sh                    # matcher regression on stored captures (see below)
```

The driver warns about nothing today; keep it that way (`-Wall -Wextra`).

**Releasing a change.** Bump `pkgrel` in `packaging/PKGBUILD`, build with
`makepkg -f`, then check that `packaging/src/libfprint/libfprint/drivers/goodix5125/*`
is byte-identical to `lib/` + `driver/` (`cmp`). The owner installs it with
`./install.sh`. Agents have no sudo. Never ask for the password: give the
owner one ready-to-paste command and wait for them to report back.

## Live testing (after the owner installed a build)

The device belongs to fprintd. The user may call its verify methods without
a password, so these tools need no root:

| tool | does |
|---|---|
| `tools/cancelrace.py [delays…]` | VerifyStart, then VerifyStop and the next request back to back, the way GNOME Settings does. Every line must say `ok`. No finger needed, changes nothing |
| `tools/latency.py N` | timing: sensor ready / touch → result / result → action closed. Prompts the owner by desktop notification |
| `tools/verifyloop.py N [label]` | N guided `fprintd-verify` runs. With debug on it also prints the driver's scores |
| `tools/fprintd-debug.sh on\|collect\|off` | (owner runs, sudo) fprintd drop-in: driver debug log to the journal + every capture dumped under `/var/lib/fprint/goodix5125-dump`. `collect` copies the dumps to `captures/fprintd`. **Turn it off afterwards** |
| `journalctl -u fprintd` | warnings from the driver, e.g. `sensor not answering …, recovering it` |

Prompt the owner with `notify-send` whenever a finger is needed. That has
worked well.

## Driver architecture (`driver/goodix5125.c`)

- Every action (open/enroll/verify/identify) is a `Job` run in a GTask worker
  thread that executes the blocking lib sequence. Results go back to the main
  context: `post()` for finger status and enroll progress, `post_result()` for
  the match result.
- One TLS session per action: `session_open` (init + TLS + finger-free
  background, ~0.8 s), then capture/match, then `gx_sensor_close`.
- `self->current` is the job whose libfprint action is still open. It is
  touched only on the main context. A `serial` lets late notes from a stale
  worker be dropped.
- **Actions end early, workers finish late.**
  - `dev_cancel` completes the action at once with `G_IO_ERROR_CANCELLED`.
  - A verify/identify ends as soon as its result exists (`note_cb` →
    `complete_job`). The worker then waits for the finger to lift in the
    background (`wait_after_result`, ≤3 s, ignores cancellation).
  - `worker_lock` makes the next job's worker wait for the previous one, so
    a miss never re-captures the same resting finger.
  - `dev_close` defers until all workers are done and cancels `self->abort`
    to hurry them.
- Cancellation reaches USB through `job->io`, a GCancellable that GUsb
  transfers take. It is cancelled by the action's cancellable (unless
  `ignore_cancel` is set) and by `abort`.
- Prints are `FPI_PRINT_RAW`. `fpi-data` holds the GVariant `"(qay)"`:
  `PRINT_VERSION` plus the raw `gx_template` array (15 cropped 64×80 images).
  Changing `gx_template` or preprocessing invalidates stored prints: bump
  `PRINT_VERSION` and tell the owner to re-enroll.
- Enrollment: 15 stages. Each touch only has to be of good quality (coverage
  ≥ 0.75, ≥ 20 keypoints). **Do not require touches to overlap earlier ones**:
  the sensor sees ~4×5 mm, and such a gate stalled GNOME enrollment at 2/15.
- Env vars (for development under fprintd): `GOODIX5125_PSK` (key path),
  `GOODIX5125_TRACE` (hex-dump frames), `GOODIX5125_DUMP=dir` (write PGMs).

## Invariants: each one broke the real device before

1. **Never cut a USB read short with a timeout mid-transfer.** On a timeout
   GUsb reports `actual = -1`, so the bytes that had arrived are lost and the
   pack stream breaks ("protocol error"). Driver reads use the full timeout
   plus a GCancellable (`gx_transport.interruptible = true`). Only the CLI
   transport polls in slices.
2. **Never cancel a USB write.** The sensor would read the next packs as the
   rest of the half-sent one.
3. **Always send `TLS_ESTABLISHED` (0xd4) when closing a session, even after a
   cancel.** A sensor left mid-handshake refuses plain commands, even across
   power cycles. `job->closing` makes the close uncancellable.
   `session_open` retries once after `gx_sensor_unstick()`. Expect an
   occasional "recovering it" warning after aggressive cancels; that is fine.
4. **On open failure, release the interface and `g_usb_device_close()`.**
   libfprint leaves the GUsb device open, and every later Claim fails with
   "already open" until fprintd restarts.
5. **Complete cancelled or answered actions immediately.** GNOME Settings
   sends VerifyStop and Delete/EnrollStart back to back without waiting.
   fprintd refuses with "already in progress" (shown as "device is claimed by
   another process") while the old action is open. fprintd's VerifyStop also
   waits up to 1 s for the device after a status report.
6. The background must be finger-free. `session_open` validates it and falls
   back to the last clean one (`self->bg`).
7. Never write the sudo password, `psk.bin`, or fingerprint images anywhere
   they could leave this machine (see Data below).

## Matcher (`lib/match.c`) and how to change it

Pipeline:
1. Shi-Tomasi corners (≤48) on the equalised image.
2. Oriented disc patch descriptors (radius 7). Mutual, unambiguous matches,
   tried at probe rotations −60°…+60° in 12° steps.
3. RANSAC rigid transform. The score is the inlier count.
4. Confirmation: warp the probe onto the view and compute the NCC over the
   overlap. It must reach `GX_NCC_MIN` 0.55 over ≥ `GX_OVERLAP_MIN` 600 px,
   or the score is 0.
5. `gx_match_set` takes the best over the enrolled views. Accept when the
   score ≥ `GX_MATCH_THRESHOLD` (4).

Why step 4 exists: different fingers reach 4-7 chance inliers, but their
aligned ridges do not correlate (≤ 0.25 at ≥ 4 inliers). Genuine pairs give
0.65-0.95. Plain NCC without keypoints is useless (parallel ridges correlate
for any two fingers). Ridge-direction-normalised descriptors killed
discrimination. A "mosaic" stitching enrolled views was tried and removed:
real enrolments fragment, and pooling raised impostor scores.

Weak spot: a few impostor *pairs* reach 3 inliers with NCC 0.55-0.58, one
inlier below the threshold. Raising `GX_NCC_MIN` to 0.65 costs nothing on the
stored sets, but it is unverified live.

**Any matcher change must be checked with `tools/evalset.sh` before and
after.** Current baseline:

```
genuine real : 7 8 12 10 5 8 7 6 7 9 8      (all ≥ 4 → accepted)
genuine ds-ri: 11 7 5 6 9 8 5 10
impostor max : 0 3 3  (n=192)                 (none ≥ 4)
```

Never accept a change that lets any impostor reach the threshold. Live:
right index 8/10 per touch, other fingers 0/10. Misses are touches on skin
the enrolment never saw.

## Data and privacy

- `psk.bin` (project root, 0600) and `/etc/goodix5125/psk.bin` hold the
  sensor's unique TLS key. Never print, commit, upload or share it. If Windows
  re-provisions the sensor (e.g. Windows Hello), the key changes and must be
  extracted again (`vm/`, `tools/extract-psk.ps1`).
- `captures/` (0700, local only) holds the developer's fingerprint images (biometric data).
  `captures/ds/<finger>/` has 5 fingers × 8 touches. `captures/real/{enroll,probe}`
  symlinks into `captures/fprintd/` (a real fprintd enrolment and live
  probes). A fresh clone has none of this: collect your own with
  `tools/dataset.py` and `tools/fprintd-debug.sh` before touching the matcher. `build/dbg/` has older debug images. Keep all of it local and out
  of version control (`.gitignore`). Do not copy it into the repo or into
  tool output that leaves the machine.
- Stored prints in `/var/lib/fprint` contain raw template images (root-only).

## Working with the person at the laptop

Do the work end-to-end and keep their manual steps few. When a step needs
them (sudo, touching the sensor), batch it into one command or one
notification round. Report honestly what was verified on hardware and what
was not.
