# cv710userspace

An independent, reverse-engineered userspace capture driver for the AVerMedia
**ExtremeCap U3** (CV710, USB `07ca:0710`) on Linux.

> **Not affiliated with AVerMedia.** This is an independent interoperability
> project. It is not affiliated with, authorised by or endorsed by AVerMedia.
> "AVerMedia", "ExtremeCap U3" and "CV710" are trademarks of their respective
> owners, used only to identify the hardware. See [Legal](#legal).

It displays captured video and audio in a standalone SDL3 window, or forwards
the stream to a virtual V4L2 loopback device for OBS, Discord or a browser.
There is also a Windows build, described in [WINDOWS.md](WINDOWS.md).

- **Status:** experimental, forked from
  [ChrisAJS/lgx2userspace](https://github.com/ChrisAJS/lgx2userspace), provided
  without warranty. It reimplements the official driver's behaviour from
  observation; no vendor source is used.
- **Related project:** the kernel-space sibling is
  [`avmcv710`](../avmcv710), which exposes the same hardware as a standard
  capture node instead of a userspace app. The two are alternatives, so you
  need one or the other, not both. See [Setup](#setup) for running them side by
  side.
- **Docs:** [PROTOCOL.md](PROTOCOL.md) and [ISSUES.md](ISSUES.md).

## Contents

- [Component video input](#component-video-input)
- [Requirements](#requirements)
- [Building](#building)
- [Setup](#setup)
- [Running](#running)
- [V4L2 output](#v4l2-output)
- [Audio routing](#audio-routing)
- [Hardware frame-rate converter](#hardware-frame-rate-converter-cvx8897)
- [Legal](#legal)
- [Attributions](#attributions)

## Component video input

Not supported. Capture on the ExtremeCap U3 is HDMI only. The component input
needs a proprietary multi-pin breakout cable plus ADV7604 analog front-end
calibration, sync-slicer clock locking and analog audio routing through the
onboard TLV320AIC3101 codec. Switching inputs in software without those
routines only corrupts the HDMI contrast and brightness levels.

## Requirements

* CMake 3.18+
* libusb 1.0
* SDL3
* V4L2Loopback (optional, for the virtual webcam output)

## Building

```bash
# Ubuntu / Debian (24.04+)
sudo apt install cmake g++ libusb-1.0-0-dev libsdl3-dev v4l2loopback-dkms v4l2loopback-utils

# Arch Linux
sudo pacman -S cmake gcc libusb sdl3 v4l2loopback-dkms v4l2loopback-utils

# Fedora
sudo dnf install cmake gcc-c++ libusb1-devel SDL3-devel v4l2loopback
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Setup

The driver needs read and write access to the CV710 USB device. Install the
udev rules and re-plug the device:

```bash
sudo cp 99-avermedia-cv710.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

> **Coexisting with the `avmcv710` kernel driver.** This driver needs
> exclusive access to the USB interface. If the kernel V4L2 driver is bound,
> libusb detaches it on claim and re-attaches it on exit, so both can be
> installed side by side, as long as nothing is *streaming* through the kernel
> driver. If an application such as OBS is using `avmcv710` at the same time,
> the claim fails. Close it, or unload the module first with
> `sudo modprobe -r avmcv710`.

## Running

```bash
./build/src/cli/cv710userspace
```

The application initialises the hardware and streams 1080p60 uncompressed
video and audio to the SDL3 preview window. The console is quiet by default:
it prints event-driven lines only (resolution changes, lock status, drops,
errors). Add `-v` or `-V` for the throttled per-transfer and per-frame chatter.

Keys:

* `Tab` / `O`: toggle the diagnostic HUD (input, colour, audio, capture status).
* `C`: cycle colour profiles (BT.709 limited, BT.709 full range, BT.601, UYVY, raw YUY2).
* `F` / `G`: enter / leave fullscreen.
* `R`: re-anchor the parser on the next frame marker. This is a software
  re-sync only, with no hardware writes (mid-stream FPGA/FX3 resets were
  field-tested and corrupt the stream). Use it if the picture gets skewed or
  misaligned and stays that way. The parser also re-anchors automatically after
  about 1 second of continuously corrupt frames, capped at 2 attempts; if it
  is still stuck, replug the device.
* `Esc`: quit.

Resolution and colour changes show a small bottom-centre toast; the full HUD
appears only on `Tab` / `O`.

The OSD uses an antialiased TTF (Inter, Noto Sans or Fira Sans are
auto-detected). Force a specific font with `CV710_FONT=/path/to/font.ttf`, or
drop one at `assets/fonts/ui.ttf`. Without a TTF it falls back to the built-in
SDL debug font.

Flags:

* `-d DEVICE`: forward the stream to a V4L2 loopback device (see
  [V4L2 output](#v4l2-output)).
* `-c COLOR`: initial colours profile (`bt709`, `bt709full`, `bt601`,
  `bt601full`, `uyvy`, `yuy2`).
* `-S SCALE`: output scaling (`1` full, `2` half, `4` quarter).
* `--aspect MODE`: preview aspect handling. `stretch` (default) fills the
  window, `auto` preserves each source's natural DAR (480p/576p as 4:3, HD as
  16:9), `4:3` and `16:9` force that DAR. Non-stretch modes pillarbox or
  letterbox in black instead of squashing.
* `-g`: video only. `-s`: audio only (no video preview).
* `-v`: exit summary. `-V`: real-time timing. Both enable the throttled console
  chatter.
* `--audio-device NAME|INDEX`, `--audio-loopback`, `--list-audio-devices`: see
  [Audio routing](#audio-routing).
* `--convert MODE`, `--list-cvx8897`: see
  [Hardware frame-rate converter](#hardware-frame-rate-converter-cvx8897).

## V4L2 output

Load `v4l2loopback` and run with `-d`:

```bash
sudo modprobe v4l2loopback video_nr=99 exclusive_caps=1 card_label="CV710"
ls -l /dev/video99
./build/src/cli/cv710userspace -d /dev/video99
```

Then add a Video Capture Device (V4L2) in OBS and select `CV710
(/dev/video99)`.

## Audio routing

V4L2 carries video only, so the captured 48 kHz stereo audio is played through
SDL's default playback device. There are three ways to keep it off your desktop
speakers:

1. **`-g`**: disable audio entirely. Nothing plays, and there is no audio to
   record either.
2. **`--audio-loopback`**: route audio into a virtual sink so OBS or Discord can
   record it as a separate source. It makes the loopback a hard requirement: if
   none is found, the driver errors out instead of falling back to the speakers.
   ```bash
   sudo modprobe snd-aloop          # option A: ALSA loopback
   pw-loopback --name CV710_Audio   # option B: PipeWire null sink
   ./build/src/cli/cv710userspace -d /dev/video99 --audio-loopback
   ```
3. **`--audio-device NAME|INDEX`**: point at an explicit device, for example a
   null sink or a specific capture endpoint.
   ```bash
   ./build/src/cli/cv710userspace -d /dev/video99 --audio-device "Loopback"
   ```

With `-d` and no explicit device, a loopback sink is *preferred*; if none is
present the driver falls back to the desktop default (with a hint in the log)
rather than dropping the audio. `--audio-loopback` upgrades that to a hard
requirement. `--list-audio-devices` shows what SDL can see, with loopback
devices marked `<-- loopback (capturable)`.

To record it, point your app at the **capture side** of the sink, not the
monitor you would listen through:

* **snd-aloop**: an ALSA audio input at `Loopback: Loopback PCM`.
* **PipeWire** (`pw-loopback` / `CV710_Audio`): the device's **Monitor** input,
  or use `pavucontrol` to route the sink's monitor stream.

Video and audio then arrive as two independent sources: video from
`/dev/video99`, audio from the loopback monitor.

## Hardware frame-rate converter (CVX8897)

`--convert MODE` drives the CVX8897, the chip between the HDMI receiver and the
capture path. `native` (the default) leaves it untouched; `half` and `auto`
halve the delivered rate for the detected input (1080p60 to 30p, 1080p50 to
25p, 720p60 to 30p, and so on); any mode name from `--list-cvx8897` forces a
specific table. Geometry is unchanged, so the parser still classifies
correctly. Bench-verified: a 1080p60 source captures at about 30 fps while the
HUD's `INPUT` row still reports 60 Hz.

A few things are worth knowing:

* The converter latches its mode across app runs; only a reset or a power cycle
  clears it. If you ran `--convert half` and then launch without `--convert`,
  you still get about 30 fps. Run `--convert reset` (or power-cycle) to return
  to native 60 fps passthrough.
* The converter is re-programmed whenever the detected input changes, not just
  at startup. Modes with no table (SD, interlaced, 24p/30p and unregistered
  geometry) fall back to native passthrough instead of leaving the previous
  table latched. A live re-program replays a roughly 3 second reset pulse, so
  each convertible-mode switch shows a brief re-lock.
* The vendor sequence (`SetMode()` / `HWReset()`, a reset pulse through the
  PCA9539 GPIO expander, plus the output enable) runs **before** the
  251-register table, because the table alone leaves the converter's output PLL
  dead. The first apply completes before streaming starts.
* `--convert reset` is the recovery mode: it replays the `HWReset` preamble and
  restores native output without waiting for a lock. Use it if an experiment
  left the converter in a bad image and the source will not lock (a physical
  power cycle is the guaranteed fallback).

## Protocol documentation

The USB bulk streaming protocol, framing markers, FPGA checksum validation and
audio format are documented in [PROTOCOL.md](PROTOCOL.md). Known hardware
limitations are tracked in [ISSUES.md](ISSUES.md).

## Legal

*Not legal advice. If any of this matters to you, talk to a lawyer.*

- **No affiliation.** Independent interoperability project, not affiliated
  with, authorised by or endorsed by AVerMedia. "AVerMedia", "ExtremeCap U3"
  and "CV710" are trademarks of their respective owners, used only to identify
  the hardware this project targets.
- **Reverse-engineered for interoperability.** Written by observing the
  device's own USB traffic and the vendor's shipped binaries. It contains no
  vendor source code.
- **No HDCP, no circumvention.** The project implements no HDCP: no
  authentication, no key material, no cipher, no key derivation. The bootstrap
  sequence initialises the receiver for raw digitisation only. Whatever a
  protected source does here, it does because of the hardware, not because of
  this code. See [ISSUES.md](ISSUES.md) section 6.
- **Your responsibility.** Whether you may capture, record or redistribute any
  particular signal is governed by laws such as the DMCA (17 U.S.C. 1201) and
  the EU Information Society Directive (Art. 6) and by the terms of the
  content's licence. That is between you and the rights holders.
- **Vendor assets.** The standby overlay bitmaps in `assets/` are AVerMedia's
  artwork, reproduced so the presentation matches the official driver.
  Copyright remains with AVerMedia; remove them if that is a problem for your
  distribution.
- **No warranty.** Provided "as is", without warranty of any kind. See
  [LICENSE.md](LICENSE.md).

## Attributions

Forked from [lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) by Chris
Sawczuk (ChrisAJS), and built on:

* [libusb](https://libusb.info/)
* [SDL3](https://www.libsdl.org/)
* [V4L2Loopback](https://github.com/umlaeute/v4l2loopback)
* [Catch2](https://github.com/catchorg/Catch2)
* [AVerMedia](https://www.avermedia.com/) for the ExtremeCap U3 hardware and
  the standby overlay artwork in `assets/` (see [Legal](#legal))
