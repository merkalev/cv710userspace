# Devlog 04: The CVX8897 Frame-Rate Converter — Register Tables and Transport

This is the follow-up to devlog 03 §5. The CVX8897 is the video processor that
sits between the ADV7604 HDMI receiver and the FX3 capture path on the CV710.
The official application drives it through the *Video Input Frame Rate*
`[60p]/[30p]/[50p]/[25p]` selector, i.e. it **halves the delivered frame rate in
hardware**. This devlog records the register tables and, crucially, the I2C
transport that makes them reachable from this project.

## 1. Sources and method

Two vendor builds of the same codebase were used:

| Build | File | Notes |
|---|---|---|
| macOS | `libC877Driver.dylib` (Mach-O **i386**) | full symbol table — primary source |
| Windows | `avmu3_x64.sys` (PE **x64**) | independent byte-for-byte cross-check |

Both emit the same function-name strings (`%s  +, 0719`, `%s  -`) and the same
`CVX8897::REG_*` symbols, so they are the same code compiled differently.

Each `CVX8897::REG_<mode>(...)` function is a straight-line, unrolled sequence of
`CVX8897::WriteRegister(reg, val)` calls. Tooling (in-repo at
`tools/cvx8897/`):

* `extract_cvx8897.py` — emulates enough i386 (immediates + `pushl` + the
  `calll` into `WriteRegister`) to recover every `(reg, val)` pair.
* `xcheck_cvx8897_win.py` — recovers the same stream from the Windows x64
  sequence (`movb $reg,%dl; movb $val,%r8b; movq this,%rcx; callq
  CVX8897::WriteRegister`) and compares.

Result: **14 modes matched, 0 mismatched.**

## 2. Shape of the tables

Every mode performs exactly **251 `WriteRegister` calls**:

* registers `0x01..0xff` ascending, 250 writes, skipping the reserved
  `0x15, 0x1c, 0x74, 0xb9, 0xba`;
* then a final **commit write to register `0x01`**.

The register order is identical for all 14 modes, so the data is stored once
(`tools/cvx8897/cvx8897_tables.json`: `reg_order` + one value array per mode).
Applying a mode is:

```cpp
for (size_t i = 0; i < cvx8897::kRegCount; ++i)
    sendI2cWrite(cvx8897::kI2cAddress, cvx8897::kRegOrder[i], table[i]);
```

The 1080p60→60p **passthrough** table differs from the 1080p60→30p table in
only 14 registers:
`0x07, 0x08, 0x0a, 0x10, 0x3a, 0x45, 0x81, 0x8c, 0x98, 0xa5, 0xa6, 0xaa, 0xaf,
0xf0` — the rest is the shared block that sets up the converter.

## 3. Transport — the CVX8897 *is* reachable from the existing EP1 bridge

This was the one open risk in devlog 03. Resolved:

* `CVX8897::WriteRegister(reg, val)` builds a bare two-byte I2C write
  `[reg, val]` and calls `CI2CBus::I2CSubSend(slave = this->addr = **0x30**,
  sublen = 0, data = {reg,val}, datalen = 2, flag = 0)`.
  (`this->addr` is set to `0x30` in the CVX8897 constructor — the vendor uses
  **8-bit** I2C addresses internally.)
* `UsbI2cInterface::write` forwards to `Fx3Firmware::I2cWrite(slave, len, buf,
  flag, timeout)`, which builds the on-wire command:

  ```
  cmd[0] = flag ? 0x03 : 0x01
  cmd[1] = slave_8bit >> 1        // 7-bit address
  cmd[2] = 0x01                   // (0x02 if slave == 0xA0)
  cmd[3..] = data
  ```

  So a CVX8897 write is `01 18 01 <reg> <val>` (because `0x30 >> 1 = 0x18`).

That is **byte-for-byte** what the userspace `UsbStream::sendI2cWrite(0x18, reg,
val)` emits on EP1 bulk OUT. The captured bootstrap already shows the CVX8897
being initialised this way — its `SetAllOutputMode` writes are present as:

```
>0118018A02    # 0x18 reg 0x8A = 0x02
>0118010DA4    # 0x18 reg 0x0D = 0xA4
```

matching `SetAllOutputMode`'s registers `0x8a`/`0x0d`. **Conclusion: the
CVX8897 is at 7-bit `0x18` on the existing bridge; no new transport code is
needed.**

## 4. Vendor API

| Symbol | Role |
|---|---|
| `SetParam(VX8879_PARAM)` | jump table over params 1..14 → the 14 `REG_*` functions |
| `SetMode(VX8897_MODE)` | power / HW reset / output enable |
| `SetAllOutputMode(VX8879_OUTPUT_MODE)` | writes regs `0x8a` / `0x0d` |
| `HardwareInit()` | empty — the FX3 firmware leaves the converter usable |
| `HWReset()` | reset |

`SetAllOutputMode` is already exercised by the captured bootstrap, which is why
the converter is in a defined state even without our intervention.

## 5. Implementation (userspace opt-in, CV-28)

The userspace driver wires the converter behind a default-off flag:

```
--convert native | half | auto | <mode name>     (--list-cvx8897 enumerates)
```

`cvx8897::selectHalfRate(width, height, fps, rgb, interlaced)` maps the detected
input to a table (`1920x1080` 60→30 / 50→25, `1280x720` 60→30 incl. a dedicated
RGB table / 50→25, plus the 1680×1050 / 1440×900 / 1360×768 / 1280×1024 /
1280×800 / 1280×768 / 1024×768 / 800×600 60→30 modes; `1366x768` is served by the
`1360x768` table), and `cvx8897::apply()` issues the 251 writes through
`UsbStream::sendI2cWrite(0x18, …)`. It is applied **once**, on the first status
snapshot that has the input locked (explicit mode names skip the lock check).
The kernel V4L2 driver will follow once this is bench-verified.

* **Apply at setup, not mid-stream.** The vendor selects a mode once; there is
  no evidence that rewriting the tables while frames are flowing is safe, so the
  upload happens on the initial locked snapshot (before the first frames are
  consumed in practice).
* **Geometry is unchanged.** After conversion the frame still contains the full
  source pixel count (the converter halves the *rate*, not the size), so the
  parser's word-count band still identifies 1080p/720p/… exactly as before.
* **Delivered fps halves; the ADV7604 still reports the source rate.** After
  `1080p60 → 30p`, live captured fps ≈ 30 while the receiver's `measuredFps`
  still says 60 — the two legitimately disagree, and the HUD should make that
  visible rather than "correcting" one to the other.
* **Mode selection** must be derived from the detected input geometry + rate,
  e.g. 1080p + 60 Hz → `1920_1080_60P_422_To_1920_1080_30P_422`. The
  registered list (devlog 03 §4) is the authoritative set.

## 6. HDCP status readout (documented, not implemented)

Recorded here for completeness; **the decision is to not implement it** (see
`TODO.md`, `ISSUES.md` §6, `PROTOCOL.md` §8 — the device is used on unencrypted
HDMI and sources must keep streaming freely).

The vendor *does* have the read path, should it ever be wanted:

* `CRXADV7604::ADIAPI_RxHdmiIsContentEncrypted(port, bool*)` wraps
  `CRXADV7604::HAL_RxEncrypted(port)`, which reads a receiver status byte with
  `ATV_I2CReadField8(...)` and cross-checks cable-detect (IO `0x20` / `0x6f`).
* The ADV7604 **HDCP map** is 7-bit I2C `0x3A` (the vendor's 8-bit `0x74`, as
  listed in its receiver map-address table), consistent with the I2C convention
  documented for the other maps in devlog 03 §6.

Implementing it would mean polling that map and publishing an encryption flag in
`VideoSignalInfo`, then selecting the vendor HDCP/content-protection overlays.
Deliberately not done.

## 7. Open questions

* Whether the converter needs a specific `SetMode`/`SetAllOutputMode` ordering
  around a table upload (the bootstrap's `0x8a`/`0x0d` writes suggest the
  output-enable bits are part of it).
* Whether writing a table while the source is locked is stable — bench work.
* Interaction with the passthrough default: confirming the converter is
  currently in the passthrough image (or is simply transparent) on a cold boot.
