# Devlog 02: Hardware Mysteries, Performance Optimizations, and Sub-1080p Analysis

## Credits
Based on the foundational userspace capture driver by ChrisAJS (https://github.com/ChrisAJS/lgx2userspace). This devlog documents performance optimizations, hardware architecture anomalies, protocol mysteries, and ongoing resolution investigations for the AVerMedia ExtremeCap U3 (CV710).

---

## 1. Current Milestone: Rock-Solid 1080p60 Capture

Following recent optimizations, 1080p60 capture has reached complete real-time stability:
- **Throughput**: Locked 60.0 fps sustained (`1036800 uint32s` per frame).
- **USB Queue Stability**: `dropped=0`, `qdepth=1` across thousands of bulk transfers.
- **Audio Integrity**: Uninterrupted 48 kHz stereo audio with zero popping or dropouts.
- **CPU Utilization**: Slashing frame conversion time from ~12 ms down to ~1 ms via row-sliced multi-threading across `std::thread::hardware_concurrency()` cores.

---

## 2. The Weirdest and Most Complicated Hardware Findings

Reverse engineering the official Windows (`avmu3_x64.sys`) and macOS (`libC877Driver.dylib`) drivers alongside USB protocol dumps revealed several peculiar design decisions in the CV710:

### 1. Bypassing Native UVC in Favor of Custom Bulk EP3 Framing
The Cypress FX3 (CYUSB3014) USB 3.0 controller includes built-in hardware support and reference firmware for the standard USB Video Class (UVC). AVerMedia deliberately abandoned standard UVC. Instead, custom FX3 firmware streams uncompressed YUY2 macropixels over a generic bulk endpoint (`LIBUSB_ENDPOINT_IN | 0x03`) using a custom packet trailer protocol:
- Frame start header: `0xC0FFFF00` followed by a sequence and mode metadata word.
- Frame end trailer: `0xC1FFFF00` followed by a status word and checksum.

### 2. Audio Multiplexed Directly Inside the Video Data Stream
Most capture devices stream audio over a dedicated USB Isochronous or Bulk Audio endpoint (`0x84` or standard USB Audio Class). The CV710 multiplexes 48 kHz 16-bit stereo PCM audio directly into the video bulk stream on Endpoint 3:
- The Cypress FX3 DMA controller pauses pixel transmission, injects an audio packet prefixed by `0xA0000000` (followed by a length word and PCM samples), and then resumes video transmission.
- Software must strip these audio packets out of the stream on the fly. Any failure to parse these headers instantly corrupts the video frame with visual noise.

### 3. The 350-Step Hexadecimal Bootstrap "Dance"
On power-up, the FX3 ARM9 core does not autonomously configure the Analog Devices ADV7604 digitizer. The host driver must manually issue over 350 individual vendor control transfers (`0x01` I2C bridge command) across multiple I2C slave maps (`0x20` IO, `0x22` CP, `0x64` KSV, `0x68` HDMI, `0x6C` EDID) to bit-bang PLL settings, equalize HDMI differential pairs, tune termination resistors, and configure the pixel bus.

### 4. The Missing Hardware EDID EEPROM
The CV710 has no dedicated physical EDID EEPROM on its HDMI input port. If an HDMI source is connected while the capture driver is closed, the source detects nothing. Only when the host driver writes a 256-byte EDID payload into the ADV7604 internal RAM map (`0x6C`) does the source console or PC recognize an "AVerMedia HD Capture" monitor.

### 5. The False 720p Classification Hazard
A standard 1080p frame consists of `1,036,800` 32-bit words, while 720p consists of `460,800` words. When host CPU scheduling or USB bus congestion causes a bulk transfer to drop, the Cypress FX3 does not discard the runt frame: it simply streams whatever is in the GPIF II FIFO until the next `C0` sync marker. An interrupted 1080p frame produces roughly 450,000 to 600,000 words. Because this matched naive 720p size thresholds, earlier driver logic falsely classified interrupted 1080p frames as 720p, triggering rapid texture reallocation, thread starvation, and crash loops. Confining detection to strict tolerance bands and adding 5-frame hysteresis resolved this behavior.

### 6. The Audio Muting Trap
Earlier implementations invoked `_audioOutput->clearAudio()` whenever a transport sequence gap occurred (`b0 != _lastSeq + 1`). This flushed the SDL audio playback buffer on every single dropped video frame, starving the audio playback device and creating continuous stuttering and audio popping. Removing the audio clear on dropped video packets restored smooth, uninterrupted audio playback.

---

## 3. Unsolved Hardware Mysteries

1. **The Undocumented `0x38000000` Suffix Word**:
   Immediately following the `0xC1000000` frame trailer, the FX3 firmware always outputs a 32-bit word matching `(word & 0xFF000000) == 0x38000000`. The lower 24 bits remain undocumented: hypotheses include a hardware microsecond timestamp, an ADV7604 horizontal sync line count, a CRC checksum, or a GPIF II FIFO underflow flag.

2. **Dormant Endpoint `0x82`**:
   The USB descriptor exposes an IN endpoint `0x82`. The official Windows driver issues a single 64-byte query during startup, but never touches it again during active capture. It remains unclear whether this was intended for factory testing, an unreleased IR remote control receiver, or an abandoned hardware telemetry channel.

3. **Variable Post-Trailer Zero Padding**:
   Depending on whether the incoming HDMI stream is 59.94 Hz or 60.00 Hz, the number of trailing padding bytes after `0xC1` varies from 8 bytes to 32 bytes before the next `0xC0` header begins.

---

## 4. Sub-1080p Resolution Analysis (Status and Investigation for Tomorrow)

### Observed Behavior
While 1080p60 captures at 60.0 fps with zero queue drops, resolutions under 1080p (such as 720p, 576p, or 480p) do not yet work. Log outputs continue reporting `size: 1036800 uint32s` even when a sub-1080p source is connected, or fail to produce valid locked frames.

### Potential Root Causes Under Investigation
1. **Hardcoded 1080p60 EDID Block**:
   The 256-byte EDID uploaded to ADV7604 I2C map `0x6C` during bootstrap hardcodes 1920x1080@60Hz as the primary detailed timing descriptor. Connected devices (PCs or consoles) may read this descriptor and refuse to output native 720p, or upscale/downscale internally.
2. **Static ADV7604 `VID_STD` Register**:
   During initialization, the driver writes `VID_STD = 0x06` (1080p60) to IO map `0x20`. In the ADV7604 architecture, receiving 720p60 requires setting `VID_STD = 0x13`, 576p requires `VID_STD = 0x0B`, and 480p requires `VID_STD = 0x0A`. If `VID_STD` is left fixed at 1080p, the ADV7604 HDMI receiver and clock recovery PLL cannot track sub-1080p pixel clocks.
3. **ADV7604 STDI (Standard Identification) Monitoring**:
   In the official driver, `CRXADV7604` polls Standard Identification registers to detect incoming line count and field frequency, dynamically updating `VID_STD` and notifying the Cypress FX3 state machine.

### Action Plan for Tomorrow
- Disassemble `CRXADV7604::CheckVideoStandard` and `ParseAVerData::updateResolution` in the vendor driver.
- Read ADV7604 STDI registers (`0x20` map `0x88` through `0x8E`) over the Cypress FX3 I2C command to inspect line counts on 720p/480p signals.
- Test dynamically updating `VID_STD` upon signal standard changes.
- Construct a multi-resolution EDID table supporting 1080p, 720p, 576p, and 480p timings.
