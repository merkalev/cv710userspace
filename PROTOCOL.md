# USB Data Protocol: AVerMedia ExtremeCap U3 (CV710)

## Credits
Based on protocol research by ChrisAJS (https://github.com/ChrisAJS/lgx2userspace). This document details the USB bulk streaming protocol, packet framing, and checksum verification for the AVerMedia ExtremeCap U3 (CV710).

---

## 1. Protocol Overview and Hardware Profile

The AVerMedia ExtremeCap U3 (CV710 / C877, USB ID `07ca:0710`) is a SuperSpeed USB 3.0 capture device based on the Cypress FX3 USB controller, Analog Devices ADV7604 HDMI receiver, and a Lattice ECP3 FPGA for frame serialization.

### Hardware Specifications
- USB Bulk Endpoint: `IN 0x83` (`LIBUSB_ENDPOINT_IN | 0x03`)
- Transfer Chunk Size: 2,080,768 bytes (`0x1FC000` bytes = 520,192 uint32 words)
- Native Pixel Format: YUY2 (YCbCr 4:2:2, 2 bytes/pixel, 1 macropixel per uint32)
- 1080p Active Frame Size: Exactly 1,036,800 uint32 words (1920 x 1080 / 2)
- Video Start Marker: `0xC0FFFF00` + 1 metadata word (with FPGA checksum)
- Video End Marker: `0xC1FFFF00` + 2 trailer words (with trailer checksum)
- Audio Start Marker: `0x58FFFF00` + big-endian length word
- Audio Trailer / Padding: `0xAA5555AA` padding to 32 KB DMA boundary

---

## 2. Framing Markers (uint32 Little-Endian)

| Name | Hex Value | Role |
|---|---|---|
| `VIDEO_FRAME_START_MARKER` | `0xC0FFFF00` | Opens an active video frame sub-chunk |
| `VIDEO_FRAME_END_MARKER` | `0xC1FFFF00` | Closes an active video frame sub-chunk |
| `AUDIO_FRAME_START_MARKER` | `0x58FFFF00` | Opens an audio packet |
| `AUDIO_FRAME_END_MARKER` | `0xAA5555AA` | Audio packet termination and DMA alignment padding |

None of these values represent legal YUY2 limited-range pixel pairs:
- `0xC0FFFF00`: Y0=0x00 (illegal black), Cb=0xFF (extreme blue), Y1=0xFF (illegal white), Cr=0xC0 (extreme red).
- `0xC1FFFF00`: Similar out-of-gamut coordinate.
- `0x58FFFF00`: Out-of-gamut coordinate.
- `0xAA5555AA`: Y0=0xAA, Cb=0x55, Y1=0x55, Cr=0xAA.

---

## 3. CV710 Hardware Framing and FPGA Checksum

Disassembly of the official macOS driver (`libC877Driver.dylib`) and Windows kernel driver (`avmu3_x64.sys`) revealed the exact framing and validation algorithm implemented by the hardware FPGA.

### The C0 Header Structure
A valid CV710 video header consists of exactly 2 uint32 words:
```
Word 0: 0xC0FFFF00  (VIDEO_FRAME_START_MARKER)
Word 1: [b3][b2][b1][b0]  (32-bit metadata word)
```

The metadata bytes are defined as follows:
- `b0`: Sequence counter. The FPGA emits `0x01` through `0xFF` and **never emits `0x00`**, incrementing by 1 per frame sub-chunk and wrapping `0xFF` → `0x01`. (Verified by logging every C0 across multiple full cycles on real hardware.)
- `b1`: Fixed flag byte, must equal `0x01`.
- `b2`: Field and scanning mode flags:
  - `b2 & 0x80`: Dual-field interlaced video indicator (1 = interlaced, 0 = progressive).
  - `b2 & 0x01`: Field index (0 = Field 0, 1 = Field 1).
- `b3`: Hardware FPGA Checksum byte.

### The FPGA Checksum Formula
To verify that a `0xC0FFFF00` marker is a genuine sub-chunk header and not accidental pixel data, the FPGA enforces the following checksum relation:
```cpp
uint8_t calculated = (b0 + b1 + b2 - 0x40) & 0xFF;
bool isValidHeader = (b1 == 0x01) && (calculated == b3);
```
If this condition evaluates to true, the header is verified genuine. If false, the word is treated as standard video payload.

### Sequence Continuity and Resynchronization
The sequence counter wraps `0xFF` → `0x01` rather than through `0x00`, so the expected next value is `nextSeq(last) = (last == 0xFF) ? 0x01 : last + 1`. When `b0 != nextSeq(lastSeq)`, a transport gap has occurred (typically caused by heavy CPU load or USB host transfer drops). The parser immediately:
1. Discards the current partial video frame.
2. Clears the audio accumulation buffer.
3. Resynchronizes to the new C0 boundary.
4. Mutes audio until consecutive clean frames arrive.

A genuine loss that straddles the wrap (e.g. `0xFF` → `0x02`) is still detected as a gap.

### The C1 Trailer Structure and Validation
When an active video frame completes, the device sends the C1 trailer:
```
Word 0: 0xC1FFFF00  (VIDEO_FRAME_END_MARKER)
Word 1: 0x38xx02xx  (trailer word 1: [b3][b2][b1][b0])
Word 2: 0x000000xx  (trailer word 2: [b7][b6][b5][b4])
```

Byte assignments reversed from `ParseAVerData::updateVideoEndInfo`:
- `b0`: Sequence number, must match the C0 sequence number (`b0 == c0_seq`).
- `b1`: Fixed flag byte, equals `0x02`.
- `b2`: Hardware sync status flag:
  - `0x04`: HDMI clock and raster are locked (stable 1080p frame).
  - `0x14`: HDMI clock is syncing / PLL recovery active (partial or adjusting frame).
- `b3`: Fixed flag byte, equals `0x38`.
- `b4`: Trailer validation checksum byte.

The official driver verifies the trailer with:
```cpp
uint8_t expectedChecksum = (b0 + b1 + b2 + b3 - 0x3F) & 0xFF;
bool isTrailerValid = (expectedChecksum == b4) && (b0 == c0_seq);
```

### Active Video Frame Validation and Padding
- Standard 1080p active frame: Exactly `1,036,800` uint32 words (`1920 x 1080 / 2`).
- During HDMI sync locking (`b2 == 0x14`), the line count may be partial (e.g. 1064 lines / 1,021,440 words).
- To guarantee constant display buffer dimensions and prevent downstream shader/texture under-runs, frames with `frameWords >= MINIMUM_VIDEO_FRAME_WORDS` that are shorter than `1,036,800` words are padded with `0x80108010` (legal YUY2 studio black: Y=16, U=128, Y=16, V=128).
- Frames below `MINIMUM_VIDEO_FRAME_WORDS` are dropped.

---

## 4. Audio Packet Framing and Inter-Frame Streaming

### Audio Packet Structure
Audio packets appear strictly in the inter-frame gap between C1 and the next C0:
```
Word 0: 0x58FFFF00  (AUDIO_FRAME_START_MARKER)
Word 1: [rawLen]     (Length word containing BIG-ENDIAN byte count)
Word 2..N: [PCM Data] (16-bit signed stereo L-PCM samples)
Word N+1..M: 0xAA5555AA (Padding to 32 KB DMA alignment boundary)
```

The audio byte count is extracted from Word 1 using big-endian decoding:
```cpp
uint32_t audioBytes = (((rawLen >> 16) & 0xFF) << 8) | ((rawLen >> 24) & 0xFF);
```

### Standard Audio Payload Sizes (48 kHz Stereo 16-bit S16LE)
- 60.00 fps: Exactly 800 stereo samples = 3,200 bytes per video frame.
- 50.00 fps: Exactly 960 stereo samples = 3,840 bytes per video frame.
- 59.94 fps: Alternating 800 and 801 samples (3,200 and 3,204 bytes).

### Audio Buzzing Root Cause and Watchdog Muting
When an HDMI cable is disconnected, or the resolution or refresh rate switches:
1. The ADV7604 Audio Clock Regeneration (ACR) PLL loses lock.
2. The hardware FIFO outputs floating bus noise and repeating stale DC offsets.
3. Without a video lock gate, these noise samples are pushed to the audio output, causing loud buzzing.

The userspace driver solves this by implementing an automatic audio muting watchdog:
- Audio is muted whenever video lock is lost or when no valid frame has arrived for > 250 ms.
- On sequence gaps (`b0 != ((lastSeq + 1) & 0xFF)`), audio accumulation buffers and queues are immediately flushed.
- Audio is unmuted only after at least 2 consecutive valid video frames arrive cleanly.
- `clearAudio()` flushes pending SDL audio stream buffers upon signal loss.

---

## 5. Colorspace Conversion Architecture

### Issue: RGB Full-Range vs YUY2 Limited-Range
HDMI sources default to different color standards:
- Nintendo Switch: Limited-range YCbCr 4:2:2 (matches raw YUY2).
- Apple Mac / PC GPU: Full-range RGB (0-255).
- DirectShow / UYVY devices: Swapped chroma (U and Y inverted).

Passing an RGB stream into a raw YUY2 texture causes red/blue channel bytes to be misinterpreted as chroma and green as luma, resulting in neon pink, amber, or green tinting with horizontal scan artifacts.

### High-Performance Fixed-Point Conversion
To eliminate chromatic distortion without GPU shader overhead, the driver includes a fixed-point integer converter operating in a single pass into an `SDL_PIXELFORMAT_RGBA32` buffer:

1. **BT.709 Limited Range (Standard HDTV 1080p) - DEFAULT**:
   - Rec.709 Studio levels: Y in [16, 235], Cb/Cr in [16, 240] centered at 128.
   - Fixed coefficients (scaled by 65536): cY=76309, cRV=117489, cGU=13975, cGV=34925, cBU=138438, y_offset=16.

2. **BT.709 Full Range (PC / Mac HDMI Full Range)**:
   - Full levels: Y in [0, 255], Cb/Cr centered at 128.
   - Fixed coefficients: cY=65536, cRV=103206, cGU=12276, cGV=30679, cBU=121608, y_offset=0.

3. **BT.601 Limited Range (SDTV / Legacy Consoles)**:
   - Rec.601 Studio levels: cY=76309, cRV=104597, cGU=25675, cGV=53279, cBU=132201, y_offset=16.

4. **BT.601 Full Range**:
   - Full levels: cY=65536, cRV=91881, cGU=22554, cGV=46802, cBU=116130, y_offset=0.

5. **UYVY Swap Mode**:
   - Swaps luma and chroma bytes: word `[U, Y0, V, Y1]` -> converted to RGB.

### Co-Sited Linear Chroma Reconstruction (4:2:2 to 4:4:4)
In raw 4:2:2 YUY2 streams, horizontal chroma resolution is halved (one Cb/Cr pair per two luma pixels). Under ITU-R BT.709/BT.601, chroma is co-sited with the even pixel ($Y_0$, $x = 2k$). Flat box reconstruction applies identical chroma to both pixels, shifting color transitions by 1 pixel to the right and causing color bleed on fine text.

The userspace driver performs co-sited linear interpolation:
- Even pixel ($2k$): uses co-sited chroma $(U_k, V_k)$.
- Odd pixel ($2k + 1$): interpolates halfway between pair $k$ and pair $k + 1$:
  $U_{odd} = (U_k + U_{k+1} + 1) / 2$, $V_{odd} = (V_k + V_{k+1} + 1) / 2$.

This restores clean, centered horizontal chroma phase with zero edge bleeding.

Runtime switching is supported via the 'C' hotkey, with command-line selection via `-c COLORSPACE`.

---

## 6. Interlaced Video and FPGA Controls

### Interlaced Video Scanline Weaving
For 1080i and 480i signals, the ADV7604 transmits alternating half-height fields (Top Field and Bottom Field). The interlace bit in the C0 header metadata word (`b2 & 0x80`) indicates dual-field interlaced video. Scanlines are woven into the output buffer:
```cpp
dest[y * 2 * stride + x]       = field0[y * stride + x];
dest[(y * 2 + 1) * stride + x] = field1[y * stride + x];
```

### Lattice ECP3 FPGA Scaler Controls
The hardware FPGA contains internal registers controlling scaling and test pattern generation (`setEnableFPGAScaler`, `SetWorkMode`, `SetInterPattern`).

---

## 7. HDCP Behavior

The ADV7604 hardware chip contains built-in HDCP decryption circuitry. In the official driver, downstream repeater authentication is negotiated over control EP `0x01`.

Because the userspace bootstrap sequence initializes the ADV7604 for raw digitization without initiating downstream authentication handshakes, the ADV7604 outputs the digitized pixel stream directly to the Cypress FX3 FIFO. Sources that typically enforce HDCP stream freely over USB bulk EP `0x83`. This behavior is verified functional and intended.

---

## 8. Attributions

- **ChrisAJS** (https://github.com/ChrisAJS/lgx2userspace): Original creator, researcher, and author of `lgx2userspace`.
- **libusb team** (https://libusb.info/): SuperSpeed USB communication library.
- **SDL team** (https://libsdl.org/): SDL3 audio and video presentation library.
