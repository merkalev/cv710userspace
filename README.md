# cv710userspace

**Before continuing! This project aims to provide userspace capture support for the AVerMedia ExtremeCap U3 (CV710) capture card on Linux. The software is experimental, forked from [ChrisAJS/lgx2userspace](https://github.com/ChrisAJS/lgx2userspace), and provided without warranty.**

This project contains a userspace driver for the [AVerMedia ExtremeCap U3 (CV710)](https://www.avermedia.com/) (USB ID `07ca:0710`).

It can be used to display captured video and audio in a standalone SDL3 window or forward the captured stream to a virtual V4L2 loopback video capture device.

There is also a Windows build. Instructions on how to use this can be found in [WINDOWS.md](WINDOWS.md).

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

## Running
Once udev has been configured, run the application:

```bash
./build/src/cli/cv710userspace
```

The application will initialize the hardware and stream 1080p60 uncompressed video and audio to an SDL3 preview window.

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
* `-v`: Print diagnostic summary at exit.
* `-V`: Print real-time diagnostic timing.
* `--audio-device NAME|INDEX`: Route captured audio to a specific playback device (substring match or list index).
* `--audio-loopback`: Route captured audio to a virtual loopback sink so it can be captured by other software.
* `--list-audio-devices`: List the playback devices SDL can see and exit.

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

### Capturing the audio separately (virtual audio sink)

V4L2 carries video only, so audio would otherwise play through your desktop
speakers. To expose the captured 48 kHz stereo audio as its own source that OBS,
Discord, etc. can record, create a virtual loopback sink and point the driver at
it:

```bash
# Option A: ALSA loopback module
sudo modprobe snd-aloop

# Option B: PipeWire (module-loopback / a null sink named e.g. "CV710_Audio")
pw-loopback --name CV710_Audio
```

List what SDL can see, then run with `--audio-loopback` (or an explicit device):

```bash
./build/src/cli/cv710userspace -d /dev/video99 --list-audio-devices
./build/src/cli/cv710userspace -d /dev/video99 --audio-loopback
```

When `-d` is used the driver automatically *prefers* a loopback sink if one is
present, falling back to the desktop default otherwise. Select the capture side
of that sink (e.g. "Monitor of …" / the loopback's capture device) in your
recording software.

## Protocol Documentation
Technical details on the USB bulk streaming protocol, packet framing markers, FPGA checksum validation, and audio format are documented in [PROTOCOL.md](PROTOCOL.md). Known hardware limitations are tracked in [ISSUES.md](ISSUES.md).

## Attributions
This project is forked from [lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) by Chris Sawczuk (ChrisAJS) and uses the hard work of:

* [libusb](https://libusb.info/)
* [SDL3](https://www.libsdl.org/)
* [V4L2Loopback](https://github.com/umlaeute/v4l2loopback)
* [Catch2](https://github.com/catchorg/Catch2)
