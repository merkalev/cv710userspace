# Devlog 03: Vendor Driver Archaeology — Standby Overlays, Out-of-Range Rules, and the CVX8897 Converter

## Credits
Based on the foundational userspace capture driver by ChrisAJS
(https://github.com/ChrisAJS/lgx2userspace). This entry records what the
extracted official Windows/macOS driver package (`avmu3_x64.sys` /
`libC877Driver.dylib`) reveals about the CV710 and how each finding maps onto
this codebase. It is a source-analysis note, not a new protocol discovery.

---

## 1. Source of the Data

The vendor installers were unpacked into a `driver_extracted/` tree
(`win/avmu3_x64.sys`, `win/aver_custom_*.bmp`, `mac/`). `strings` on the
Windows kernel module confirms the chipset we already reverse-engineered —
Cypress FX3 + **Analog Devices ADV7604** HDMI receiver + **CVX8897** video
processor — and exposes the internal class layout, which is what makes the
findings below readable. The binary's register *tables* are still opaque
data blobs (see §5); only the symbol names and the human-readable status
strings are recoverable without disassembly.

---

## 2. Standby Overlay Inventory

The package ships four 640x480x24 BMP overlays that the official app renders
instead of the frozen last frame when there is no usable picture:

| File | Meaning |
|---|---|
| `aver_custom_no_signal.bmp` | Receiver not locked / no cable |
| `aver_custom_out_of_range.bmp` | Receiver locked, but geometry cannot be captured |
| `aver_custom_hdcp_protection.bmp` | HDCP handshake failed / protected stream |
| `aver_custom_content_protection.bmp` | Protected content, capture not permitted |

All four sit in `assets/` in this repo. Until now only the no-signal bitmap
was ever loaded, so an out-of-range source (4K, over-budget PC timing) looked
identical to an unplugged cable. CV-27 wires the second state:

- **Userspace SDL path** — `SdlVideoOutput::loadSplashBitmaps()` now loads
  **no-signal** and **out-of-range** (slots 0 and 1) and
  `renderSplashScreen()` picks between them from
  `VideoSignalInfo::outOfRange`.
- **Kernel V4L2 port** — the same two overlays are embedded as YUY2 and
  blitted into queued vb2 buffers after 250 ms of no recognized frames
  (see the kernel repo's `avmcv710-status.c`).

The HDCP/content-protection bitmaps are intentionally **not** loaded: nothing
in either driver reads the ADV7604 HDCP map yet, so there is no state to key
them on. Adding them means first verifying an HDCP-authentication status
register (the vendor uses the `HDCPHandshake` / `HDCP authenticated` paths,
but the exact register is not in the strings).

---

## 3. No-Signal vs Out-of-Range: One Shared Rule

The vendor distinguishes the two the way we now do:

- **No signal** — no TMDS lock (`IO 0x6a & 0xe0 != 0xe0` or port-A bit `0x10`
  clear), or no valid status snapshot.
- **Out of range** — TMDS locked, but the measured geometry cannot be
  delivered: width outside `[320, 4095]`, height outside `[120, 2048]`, or
  `width * height / 2` over the frame-word budget (`AVM_MAX_FRAME_WORDS` =
  1,152,000), i.e. ~2.304M pixels. A 3840x2160 source fails the height
  bound; a 3840x1200 source passes the bounds but blows the word budget.

This is one predicate in two places, kept deliberately identical:

- `UsbStream.cpp` computes `outOfRange = tmdsLocked && !geometryValid`, plus
  the pixel-budget check, and publishes it as `VideoSignalInfo::outOfRange`.
- The kernel port mirrors it in `avm_signal_ok()` and turns it into the
  `AVM_SPLASH_OUT_OF_RANGE` overlay selection.

Both keep the existing double-read debounce so a flaky I2C read cannot flap
the overlay.

---

## 4. Registered Input Resolutions

The `.sys` status enum lists exactly the resolutions the vendor firmware
knows how to receive and reconfigure for:

```
640_480   720_480   720_576   800_600   1024_768  1280_720
1280_768  1280_800  1280_1024 1360_768  1440_900  1680_1050  1920_1080
```

Two things are worth noting:

- The vendor driver mostly does **not** hard-switch VID_STD per resolution;
  the strings `The Rx has been reconfigured for the detected Resolution` and
  `No Rx configuration available the detected Resolution` show it can, but
  `CRXADV7604::GetInputResolution` plus STDI measurement is the primary path.
  Our driver's stance (auto-detect via the parser's word-count bands +
  adaptive ADV7604 geometry, no mid-stream VID_STD rewrite) matches this and
  is why we stopped reprogramming VID_STD (CV-23b).
- Our adaptive path already accepts any measured geometry in range, so all of
  these resolutions — including the `1280x1024` / `1360x768` TV-box modes that
  are not in the parser's 7-mode table — now lock and stream. The table is a
  fallback, not a gate.

---

## 5. The CVX8897 Frame-Rate Converter (Tables Now Extracted)

> **Update:** the register tables have since been fully extracted and
> cross-validated — see `devlog/04-cvx8897-frame-rate-converter.md` and
> `tools/cvx8897/`. The summary below is kept for provenance.

The CVX8897 is a real video processor, and the vendor driver programs it with
per-resolution register tables whose symbol names survive in the binary:

```
CVX8897::REG_1920_1080_60P_422_To_1920_1080_60P_422   (pass-through)
CVX8897::REG_1920_1080_60P_422_To_1920_1080_30P_422
CVX8897::REG_1920_1080_50P_422_To_1920_1080_25P_422
CVX8897::REG_1280_720_60P_422_To_1280_720_30P_422
CVX8897::REG_1280_720_50P_422_To_1280_720_25P_422
CVX8897::REG_1280_720_60P_RGB_To_1280_720_30P_RGB
CVX8897::REG_1680_1050_60P_422_To_1680_1050_30P_422
CVX8897::REG_1440_900_60P_422_To_1440_900_30P_422
CVX8897::REG_1360_768_60P_422_To_1360_768_30P_422
CVX8897::REG_1280_1024_60P_422_To_1280_1024_30P_422
CVX8897::REG_1280_800_60P_422_To_1280_800_30P_422
CVX8897::REG_1280_768_60P_422_To_1280_768_30P_422
CVX8897::REG_1024_768_60P_422_To_1024_768_30P_422
CVX8897::REG_800_600_60P_422_To_800_600_30P_422
```

with the API surface `CVX8897::HardwareInit / HWReset / WriteRegister /
ReadRegister / SetAllOutputMode / SetMode / SetParam`. This is where the
official app's "Video Input Frame Rate [30p]/[25p]" options come from: it
halves the frame rate in hardware rather than dropping frames in software.

**Status in this project: tables extracted, runtime switch not yet wired.**
We capture at the source's native rate and let the host drop/convert. The
actual register byte sequences were recovered from `libC877Driver.dylib` and
cross-checked byte-for-byte against `avmu3_x64.sys`; they now live in
`tools/cvx8897/`. Applying a mode is 251 `sendI2cWrite(0x18, reg, val)` calls
(see devlog 04). The kernel port still exposes 60/50/30/25/24/15 in
`enum_frameintervals` and reports the *measured* rate via `g_parm`.

---

## 6. ADV7604 Status Registers Used by the Poll

The vendor's `CRXADV7604` status path matches the register map this project
already uses, and the kernel port now polls the same pairs at ~10 Hz:

| Register | Meaning |
|---|---|
| `IO 0x20 / 0x6a` | TMDS lock (`& 0xe0 == 0xe0`, port-A bit `0x10`) |
| `IO 0x20 / 0x12` | STDI flags (interlaced = bit 4) |
| `HDMI 0x34 / 0x07` | 12-bit active width (low nibble = high bits) |
| `HDMI 0x34 / 0x09` | 12-bit active height |
| `CP 0x22 / 0xb1`,`0xb3` | STDI blanking length / line count (telemetry cross-check) |
| `IO 0x20 / 0x6f` | cable-detect |
| `IO 0x20 / 0x01` | VID_STD |

The I2C bridge wire format these reads go over is now written down in
`PROTOCOL.md` §7 (`01 <slave7> 01 <reg> <val>` write; `02 <slave7> 01 <len>
<reg>` + EP1 IN read), because the kernel port had to re-derive it and both
drivers must serialise the command+response pair.

---

## 7. What This Changes in the Tree

- `src/liblgx/src/lgxdevice.h` — `VideoSignalInfo::outOfRange`.
- `src/liblgx/liblgx-common/src/usb/UsbStream.cpp` — out-of-range predicate,
  debounced publish, and a log suffix that now means *out of range* instead
  of merely *unlocked*.
- `src/liblgx/liblgx-common/src/sdl/SdlVideoOutput.{h,cpp}` — two-slot
  splash loading and state-based selection.
- `PROTOCOL.md` §7 — the EP1 I2C bridge command format.
- `tools/cvx8897/` — extracted CVX8897 register tables and generator
  (`cvx8897_tables.json`, `extract_cvx8897.py`, `xcheck_cvx8897_win.py`,
  `gen_cvx8897_header.py`).

The CVX8897 converter tables are extracted (§5, devlog 04) but the runtime
switch is not yet wired; the HDCP/content-protection overlays (§2) remain open.
Both are tracked as such rather than half-implemented.
