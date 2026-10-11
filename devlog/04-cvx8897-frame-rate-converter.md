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
Bench-verified in CV-28f; the kernel V4L2 driver can follow.

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

### 5.1 Bench result (CV-28e) — a standalone table upload breaks capture

The first on-hardware test **failed**, and in a way that pins down the missing
step. With `--convert half` on a locked 1080p59.94 source the log showed:

```
[CVX8897] Applied '1920_1080_60P_422_To_1920_1080_30P_422' (251 register writes).
[Video] Dropped frame while HDMI clock recovering (b2=0x10, count=1)
... (b2 cycles through 0x10..0x13 forever; never recovers)
[Video] Unmatched frameWords: 40845 (expected 1080p ~1036800)
```

The FX3 is reporting a dead/unstable pixel clock (`b2=0x10..0x13`) and the
frames are far too small to be real pictures. It does **not** recover when the
app is restarted without `--convert`; the CVX8897 keeps its 30p image until the
device is power-cycled. So writing the 251 registers on their own is not merely
incomplete — it is actively harmful.

Disassembly of the vendor caller shows why. The frame-rate change lives in
`Device::setupScaler()` (`0x437c`), whose only relevant calls are `SetMode()`
then `SetParam()`/`REG_*` (`0x24102`). `SetMode()` (`0x2405e`) first calls
`HWReset()` (`0x23f76`), which pulses the CVX8897 reset line **three times with
1000 ms waits** (via the `this->0x14` I2C-backed object, a GPIO/expander write
of "pin 4" `1→0→1`) and then sets an output-enable. Only *after* that does
`SetParam()` upload the table.

Our implementation does the `SetParam` half only. Without the `HWReset`/
`SetMode` preamble the converter's output PLL never re-locks, which is exactly
the observed symptom. **Reproducing the reset pulse is the next step** and needs
the `this->0x14` object's vtable slot 4 (`obj, 0, 4, 0, <0|1>`) resolved to a
concrete EP1 command, plus a verification pass on the bench.

Until then the raw upload is gated behind an environment variable so it cannot
corrupt a normal capture by accident:

```
CV710_CVX8897_UNSAFE=1  ./build/src/cli/cv710userspace --convert half
```

Without the variable, `--convert` prints a clear "disabled" notice and leaves
the converter native.

### 5.2 The reset resolved (CV-28f) — full vendor preamble replayed

`this->0x14` is a **`CPCA9539A`** (PCA9539 I2C GPIO expander) at 7-bit I2C
`0x74`, built in the `CVX8897` constructor alongside the `CI2CBus` (`this->0x8`)
and the `COSDep` sleep helper (`this->0x4`). Its vtable (`0x58b20`) slot 4 is
`SetGPIO(PCA9539A_PORT, uint8, PCA9539A_GPIO_DIR, PCA9539A_GPIO_VALUE)`, a
read-modify-write of the expander's config register (`0x06+port`, 1 = input)
and output register (`0x02+port`). The CVX8897 uses **port 0**:

* bit 4 = active-low **reset** line
* bit 3 = **output enable**

The vendor lowers the converter with `CVX8897::SetMode(2)` (`0x2405e`) — the
mode every 1080p→30p branch of `Device::setupScaler()` selects — which is:

1. `HWReset()`: `SetGPIO(port 0, pin 4, out, 1)` → 1000 ms →
   `SetGPIO(pin 4, 0)` → 1000 ms → `SetGPIO(pin 4, 1)` → 1000 ms;
2. `SetAllOutputMode(0)` (reg `0x8a` bit 1 clear, reg `0x0d` bit 7 clear);
3. `SetGPIO(port 0, pin 3, out, 1)`.

Only **then** does `SetParam()` upload the table. `UsbStream` now replays all of
this in `cvx8897SetModeImpl(converted)` (via `pca9539SetGpio()` /
`cvx8897SetAllOutputModeBits()`) immediately before `cvx8897::apply()`. If the
expander does not ACK the first write, the apply aborts and the converter is
left native rather than half-programmed. The three 1 s waits happen during
`initialise()` (before streaming), so they do not stall capture.

`--convert` is enabled again and no longer needs the `CV710_CVX8897_UNSAFE`
opt-in; it still needs a bench pass to confirm.

`--convert reset` is a recovery mode: it replays the same HWReset preamble with
the **native** output selection (`SetMode(1)`: `SetAllOutputMode(1)`, pin 3 = 0)
and uploads **no** table. Use it to un-stick a converter left in a bad image by
an earlier experiment, without a physical power-cycle. It does not wait for a
lock (there may be none precisely because the converter is stuck).

### 5.3 Bench result (CV-28f) — works: 60 Hz in, ≈30 fps out

With the source on, `--convert half` on a 1920×1080p60 input now logs:

```
[CVX8897] Lowering converter (SetMode/HWReset preamble)...
[CVX8897] PCA9539 port 0 before reset: out=0x71 cfg=0x00 (reset bit4, enable bit3)
[CVX8897] Applied '1920_1080_60P_422_To_1920_1080_30P_422' (251 register writes).
```

`out=0x71` means bit 4 (reset) idle-high and bit 3 (enable) low — the native
image — exactly what is expected before lowering. The ADV7604 locks and stays
locked; the CV-28e `HDMI clock recovering` storm is gone. The on-screen HUD
(`CAPTURE` row, toggle with Tab) reads **≈30 fps** while `INPUT` still reports
`1920x1080p 60.00Hz`, confirming the converter halves the delivered rate while
the receiver (which sits before it in the chain) still sees the source rate.

The whole sequence (three 1 s waits) runs during `initialise()` before
streaming. `--convert` is no longer experimental.

### 5.4 Startup ordering (CV-28i) — apply before streaming, not mid-stream

Applying the converter on the first *recognised* lock snapshot is not enough on
its own. The ADV7604's first locked reading is frequently a transient (one bench
run reported a bogus `1536x1080p @ 73.37 Hz` while the source acquired) that
matches no table, so the single up-front query in `Device::initialise()` simply
deferred the apply — and the control thread then completed it ~3 s into
capture, tearing a live stream (`Dropped frame while HDMI clock recovering`
counts climbing to 300/600, `Dropped corrupt frame ... size mismatch`).

`Device::initialise()` now polls `queryVideoSignalStatus()` every 100 ms (up to
5 s) while `Stream::cvx8897ConversionPending()` is true, **before** calling
`queueFrameRead()`. The converter is therefore fully applied — reset pulse and
table — while the FPGA is still idle, before any frames flow. The control thread
does not exist yet during this window, so the loop owns the device. If no
convertible mode appears in time the converter is left native and normal capture
proceeds; the bounded wait cannot hang startup on a missing source.

### 5.5 Live mode changes (CV-28j) — re-program per input, not once

The converter latches its mode, and the first cut programmed it **once**, on the
initial lock. Bench testing with a source cycling 1080p60 → 1080i60 → 576p50 →
720p60 → 480p60 → … left the converter stuck on the first table
(`1080p60->30p`) for every later mode, which tears the picture for all of them
(SD, interlaced and 720p). That is the "unusable in half mode" report.

`maybeApplyCvx8897()` now re-evaluates on every locked snapshot and re-programs
when the target changes. It derives a key — the selected table name, or
`native` when there is none — and:

* applies the matching table when one exists;
* restores **native passthrough** when none does (SD, interlaced, 24p/30p,
  unusual geometry) instead of leaving the previous mode latched;
* debounces a *live* change over 3 polls (~300 ms) so a transient acquisition
  geometry does not fire back-to-back 3 s resets. The initial apply (startup,
  before streaming, CV-28i) is not debounced.

`selectHalfRate()` also now refuses any rate outside the 60 Hz family
(55–66 fps) and 50 Hz family (45–55 fps), so a 24p or 30p source is never fed
the 60→30 table's input timing.

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

## 7. Resolved / remaining questions

* **Resolved (CV-28f):** `SetMode` mode `2` is the converted output and mode `1`
  the native default — every 30p branch of `setupScaler` uses mode `2` (reset +
  `SetAllOutputMode(0)` + `SetGPIO(pin 3 = 1)`), which CV-28f replays, and the
  bench confirms the halving. The pin-3 polarity is therefore correct as
  implemented.
* Interaction with the passthrough default: the pre-reset state observed on the
  bench (`out=0x71`, i.e. bit 3 = 0) matches the native image, so a cold boot is
  already native/passthrough.
* Whether the `--convert reset` recovery fully restores a wedged converter (it
  never came up in the bench session, since the only wedged state was cleared by
  power-cycling).
* **Resolved (CV-28k):** for 480p and 576p the ADV7604 reports progressive
  (720x480p / 720x576p) but the FPGA's C0 interlace bit (`b2` bit 7) is *set*
  spuriously and flaps, so the parser alternated between classifying the same
  signal as `480p`/`576p` and as a woven `720x960` / `720x1152` field pair. Each
  "field" is a full 480/576 lines (172800 / 207360 words), i.e. full frame
  height, not a half-height interlaced field — so weaving doubled the picture.
  `detectVideoMode()` now overrides the FPGA bit when the receiver is locked and
  measures a progressive frame whose word count matches exactly what the FPGA is
  sending, **unless** the count is a genuine interlaced field (1080i 518400,
  576i 103680, 480i 86400). That keeps 1080i (a 540-line field that would
  otherwise be seen as a 1920x540 progressive frame when the receiver bit is
  momentarily wrong) weaving correctly. Independent of CVX8897 conversion; the
  fix is in the parser, covered by two `device_parser` regressions.
