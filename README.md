# cv710userspace

Userspace capture driver for the [AVerMedia ExtremeCap U3 (CV710)](https://www.avermedia.com/) (USB ID `07ca:0710`) on Linux. It can display captured video and audio in a standalone SDL3 window, or forward the captured stream to a virtual V4L2 loopback device for OBS, Discord or a browser.

There is also a Windows build — see [WINDOWS.md](WINDOWS.md).

> **Status: experimental.** Forked from [ChrisAJS/lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) and provided without warranty. It reimplements the official driver's behaviour from observation — no vendor source is used — and it is neither affiliated with nor endorsed by AVerMedia (see [Legal](#legal)).

**Related project:** the kernel-space sibling is the out-of-tree V4L2 driver [`avmcv710`](../avmcv710), which exposes the same hardware as a standard capture node instead of a userspace app. They are alternatives — you need one or the other, not both; to run them side by side see [Setup](#setup).

## Contents

- [Component Video Input Notice](#component-video-input-notice)
- [Building](#building)
- [Setup](#setup)
- [Running](#running)
- [Running with V4L2 Output](#running-with-v4l2-output)
- [Protocol Documentation](#protocol-documentation)
- [Legal](#legal)
- [Attributions](#attributions)

## Component Video Input Notice
Component (YPbPr) analog video input is currently not supported. Capture on the ExtremeCap U3 is HDMI only. The component input requires a proprietary multi-pin analog breakout cable, ADV7604 analog front-end ADC calibration, sync slicer clock locking, and analog audio routing through the onboard TLV320AIC3101 codec. Attempting to switch inputs in software without these routines will only corrupt HDMI contrast/brightness levels.

## Building
To build the project, you will need:
* CMake (3.18+)
* libusb (1.0)
* SDL3
* V4L2Loopback (optional, for virtual webcam output)

### Ubuntu / Debian (24.04+)
```bash
sudo apt install cmake g++ libusb-1.0-0-dev libsdl3-dev v4l2loopback-dkms v4l2loopback-utils
```

### Arch Linux
```bash
sudo pacman -S cmake gcc libusb sdl3 v4l2loopback-dkms v4l2loopback-utils
```

### Fedora
```bash
sudo dnf install cmake gcc-c++ libusb1-devel SDL3-devel v4l2loopback
```

### Build Command
Execute the following commands in the root of the project:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Setup
The userspace driver requires read and write access to the CV710 USB device.

On Linux, permissions can be granted by adding the udev rules:

```bash
sudo cp 99-avermedia-cv710.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

After adding the rules, unplug and re-plug the CV710.

> **Coexisting with the `avmcv710` kernel driver.** The userspace driver needs
> exclusive access to the USB interface. If the kernel V4L2 driver is bound to
> the device, libusb detaches it automatically on claim (Linux) and re-attaches
> it on exit, so both can be installed side by side — as long as nothing is
> *streaming* through the kernel driver. If an application (e.g. OBS) is using
> `avmcv710` at the same time, the claim fails; close it, or unload the module
> first (`sudo modprobe -r avmcv710`).

## Running
Once udev has been configured, run the application:

```bash
./build/src/cli/cv710userspace
```

The application will initialize the hardware and stream 1080p60 uncompressed video and audio to an SDL3 preview window.

The console is quiet by default: only event-driven lines print (resolution changes, lock status, drop/re-sync events, errors). The throttled per-transfer/per-frame debug chatter is off; add `-v` or `-V` to see it.

### Options when running
- `Tab` / `O`: Toggle the persistent diagnostic HUD (rounded "glass" card: input, colour, audio and capture status).
- `C`: Cycle colorspace profiles (BT.709 limited, BT.709 full range, BT.601, UYVY, raw YUY2).
- `F`: Toggle fullscreen.
- `G`: Exit fullscreen.
- `R`: Re-anchor the parser on the next frame marker (software re-sync - no
  hardware writes; mid-stream FPGA/FX3 resets were field-tested and actively
  corrupt the stream). Use it if the picture ever gets skewed / misaligned lines
  and stays that way. The parser also re-anchors automatically after ~1 s of
  continuously corrupt frames (capped at 2 attempts per stuck episode); if the
  picture is still stuck after that, a physical replug of the capture device is
  required - software cannot re-arm the FPGA safely.
- `Esc`: Quit application.

Resolution and colour changes briefly show a small bottom-centre **toast**; the full HUD is only shown when you ask for it with `Tab`/`O`.

The OSD uses a modern antialiased TTF (Inter / Noto Sans / Fira Sans are auto-detected on the system). To force a specific font, set `CV710_FONT=/path/to/font.ttf`, or drop a font at `assets/fonts/ui.ttf`. If no TTF is found the OSD falls back to the built-in SDL debug font.

Command-line flags:
* `-c COLOR`: Set initial colorspace (`bt709`, `bt709full`, `bt601`, `bt601full`, `uyvy`, `yuy2`).
* `-S SCALE`: Output scaling factor (`1` = full, `2` = half, `4` = quarter).
* `--aspect MODE`: Aspect-ratio handling for the preview window (`stretch` = fill the window, the default; `auto` = preserve each source's natural DAR, treating 480p/576p as classic 4:3 and HD as 16:9; `4:3` / `16:9` = force that DAR). Non-stretch modes pillarbox/letterbox in black instead of squashing the picture.
* `-g`: Video only (disable audio output).
* `-s`: Audio only (disable video preview).
* `-v`: Print diagnostic summary at exit (also enables the throttled per-transfer/per-frame console chatter).
* `-V`: Print real-time diagnostic timing (also enables the throttled per-transfer/per-frame console chatter).
* `--audio-device NAME|INDEX`: Route captured audio to a specific playback device (substring match or list index).
* `--audio-loopback`: Route captured audio to a virtual loopback sink so it can be captured by other software.
* `--list-audio-devices`: List the playback devices SDL can see and exit.
* `--convert MODE`: Drive the **CVX8897 hardware frame-rate converter** (the chip between the HDMI receiver and the capture path). `native` (the default) leaves it untouched; `half`/`auto` halves the delivered rate for the detected input (1080p60→30p, 1080p50→25p, 720p60→30p, …); any mode name from `--list-cvx8897` forces a specific table. This halves the delivered frame rate in hardware (the frame geometry is unchanged, so the parser still classifies correctly), which is a different mechanism from the software 1-in-N drop. It replays the vendor's lowering sequence — `SetMode()`/`HWReset()` (a ~3 s reset pulse through the PCA9539 GPIO expander) and the output enable — **before** uploading the 251-register table, because the table alone leaves the converter's output PLL dead. The first apply completes **before streaming starts** (startup waits, bounded, for a convertible lock), so that reset pulse never tears a live stream. **Bench-verified (CV-28f): a 1080p60 source captures at ≈30 fps (HUD `CAPTURE` row, toggle with Tab) while `INPUT` still reports 60 Hz.**
  * **Live mode changes (CV-28j):** the converter is re-programmed whenever the detected input changes, not just once at startup. Modes with no table — SD (480p/576p), interlaced, 24p/30p and unregistered geometry — fall back to **native passthrough** instead of leaving the previous table latched, so switching a source through those modes keeps a clean picture. A live re-program replays the ~3 s reset pulse, so each *convertible*-mode switch shows a brief re-lock.
* `--list-cvx8897`: List the CVX8897 conversion mode names and exit.
* `--convert reset`: Recovery mode — replay the CVX8897 `HWReset` preamble and restore the **native** output (no table uploaded) without waiting for a lock. Use it if an earlier `--convert` experiment left the converter in a bad image and the source will not lock (a physical power-cycle is the guaranteed fallback).
  * **The converter latches its mode across app runs** (only a reset or a power-cycle clears it). If you ran `--convert half` and then launch *without* `--convert`, you will still get ~30 fps — run `--convert reset` (or power-cycle) to return to the native 60 fps passthrough.

## Running with V4L2 Output
### V4L2 Output Setup
To output video to a virtual webcam source (for OBS Studio, Discord, or web browsers), load the `v4l2loopback` module:

```bash
sudo modprobe v4l2loopback video_nr=99 exclusive_caps=1 card_label="CV710"
```

Verify that `/dev/video99` exists:

```bash
ls -l /dev/video99
```

### Running with a V4L2 Device
Run the userspace driver with the `-d` option:

```bash
./build/src/cli/cv710userspace -d /dev/video99
```

Open OBS Studio or your preferred streaming software, add a Video Capture Device (V4L2), and select `CV710 (/dev/video99)`.

### Keeping the audio off your desktop speakers

V4L2 carries video only, so by default the captured 48 kHz stereo audio is
played through SDL's default playback device (your desktop speakers). There are
three ways to avoid that:

1. **`-g` — disable audio entirely** (simplest if you only want the picture):
   ```bash
   ./build/src/cli/cv710userspace -d /dev/video99 -g
   ```
   No audio is opened at all: nothing plays on the desktop, and there is no
   audio to record either.

2. **`--audio-loopback` — route audio into a virtual sink** so OBS/Discord can
   record it as a separate source (recommended for webcam use):
   ```bash
   # Option A: ALSA loopback module
   sudo modprobe snd-aloop

   # Option B: PipeWire (a null sink / loopback named e.g. "CV710_Audio")
   pw-loopback --name CV710_Audio
   ```
   Then run with the flag (it makes the loopback **required** — if none is
   found the driver errors out instead of falling back to the speakers):
   ```bash
   ./build/src/cli/cv710userspace -d /dev/video99 --audio-loopback
   ```

3. **`--audio-device NAME|INDEX` — point at an explicit device**, e.g. a null
   sink or a specific capture endpoint:
   ```bash
   ./build/src/cli/cv710userspace -d /dev/video99 --audio-device "Loopback"
   ```

> **Why does it play on the desktop at all?** When `-d` is used the driver only
> *prefers* a loopback sink: if no `snd-aloop`/`loopback` device is present in
> SDL's device list it falls back to the desktop default (with a hint in the
> log) so audio isn't just dropped. `--audio-loopback` upgrades that preference
> to a hard requirement. Check what SDL sees with:
> ```bash
> ./build/src/cli/cv710userspace --list-audio-devices
> ```
> Loopback devices are marked ` <-- loopback (capturable)`.

### Capturing the audio in OBS / Discord

Add an audio input in your recording software pointed at the **capture side** of
the sink, not the monitor you'd hear yourself through:

* **snd-aloop**: add an "Audio Input Capture", source type ALSA, device
  `Loopback: Loopback PCM` (the `hw:X,1` capture subdevice).
* **PipeWire (`pw-loopback` / `CV710_Audio`)**: select the device's
  **Monitor** input (e.g. `Monitor of CV710_Audio`), or use `pavucontrol` to
  route the sink's monitor stream to your app.

Video and audio then arrive as two independent sources — video from
`/dev/video99` (V4L2), audio from the loopback monitor.

## Protocol Documentation
Technical details on the USB bulk streaming protocol, packet framing markers, FPGA checksum validation, and audio format are documented in [PROTOCOL.md](PROTOCOL.md). Known hardware limitations are tracked in [ISSUES.md](ISSUES.md).

## Legal

*Not legal advice. If any of this matters to you, talk to a lawyer.*

- **No affiliation.** This is an independent interoperability project. It is not affiliated with, authorised by or endorsed by AVerMedia. "AVerMedia", "ExtremeCap U3" and "CV710" are trademarks of their respective owners, used here only to identify the hardware this project targets.
- **Reverse-engineered for interoperability.** The driver was written by observing the device's own USB traffic and the vendor's shipped binaries; it contains no vendor source code.
- **No HDCP, no circumvention.** The project implements no HDCP: no authentication, no key material, no cipher, and no key derivation; the bootstrap sequence initialises the receiver for raw digitisation only. Whatever a protected source does here, it does because of the hardware, not because of this code — see [ISSUES.md](ISSUES.md) §6.
- **Your responsibility.** Whether you may capture, record or redistribute any particular signal is governed by laws such as the DMCA (17 U.S.C. §1201) and the EU Information Society Directive (Art. 6), and by the terms of the content's licence. That is between you and the rights holders.
- **Vendor assets.** The standby overlay bitmaps in `assets/` are AVerMedia's artwork, reproduced so the presentation matches the official driver; copyright remains with AVerMedia. Remove them if that is a problem for your distribution.
- **No warranty.** Provided "as is", without warranty of any kind — see [LICENSE.md](LICENSE.md).

## Attributions
This project is forked from [lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) by Chris Sawczuk (ChrisAJS) and uses the hard work of:

* [libusb](https://libusb.info/)
* [SDL3](https://www.libsdl.org/)
* [V4L2Loopback](https://github.com/umlaeute/v4l2loopback)
* [Catch2](https://github.com/catchorg/Catch2)
* [AVerMedia](https://www.avermedia.com/) — the ExtremeCap U3 hardware, and the standby overlay artwork in `assets/` (see [Legal](#legal))
