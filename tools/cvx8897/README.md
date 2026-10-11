# CVX8897 frame-rate converter — extracted register tables

The AVerMedia CV710 (ExtremeCap U3) contains a **CVX8897** video converter between
the ADV7604 HDMI receiver and the capture path. The official application exposes
it as the *Video Input Frame Rate* `[60p]/[30p]/[50p]/[25p]` selector: it halves
the delivered frame rate **in hardware** rather than dropping frames in software.

This directory holds the register tables recovered by disassembling the official
drivers, so the converter can be driven without re-deriving them.

## Artefact

`cvx8897_tables.json`

* `reg_order` — the 251 register writes every mode performs, in order: registers
  `0x01..0xff` (250 writes) skipping the reserved `0x15, 0x1c, 0x74, 0xb9, 0xba`,
  followed by a final commit write to register `0x01`. This order is **identical
  for all 14 modes**.
* `modes` — one value array (same length/order as `reg_order`) per mode.

Each mode is the full register image; applying a mode is simply
`for (reg, val) in zip(reg_order, mode): write(reg, val)`.

## The I2C transport (why this is usable)

`CVX8897::WriteRegister(reg, val)` emits a bare two-byte I2C write `[reg, val]`
to slave address **0x30** (the vendor's 8-bit notation). The FX3 USB bridge
`Fx3Firmware::I2cWrite` builds the on-wire command as:

```
cmd[0] = 0x01
cmd[1] = slave_8bit >> 1        # 0x30 >> 1 = 0x18  (7-bit address)
cmd[2] = 0x01
cmd[3] = reg
cmd[4] = val
```

which is byte-for-byte the userspace `UsbStream::sendI2cWrite(0x18, reg, val)`
(`01 18 01 <reg> <val>` on EP1 bulk OUT). This is confirmed by the captured
bootstrap, which already performs `SetAllOutputMode`:

```
>0118018A02   # write 0x18 reg 0x8A = 0x02
>0118010D A4  # write 0x18 reg 0x0D = 0xA4
```

The CVX8897 is therefore reachable through the **existing** EP1 I2C bridge at
7-bit address `0x18`; no new transport code is required.

## Regenerating / verifying

The tables were extracted from the vendor binaries and cross-validated:

* `../../../../driver_extracted/mac/RECentral Install Disk/RECentral.app/Contents/Resources/libC877Driver.dylib`
  (Mach-O i386, full symbol table — primary source).
* `../../../../driver_extracted/win/avmu3_x64.sys` (PE x64 — byte-for-byte cross-check).

Both builds share identical code and format strings, so the extraction is
independent and self-checking (`14 matched, 0 mismatched`).

Usage (the vendor binaries are outside this repository, so pass their paths):

```sh
# 1. disassemble
llvm-objdump -d --arch=i386 "…/libC877Driver.dylib" > /tmp/c877.dis
llvm-objdump -d --arch=x86-64 "…/avmu3_x64.sys"     > /tmp/win.dis

# 2. extract (mac) and cross-check (win)
python3 extract_cvx8897.py  /tmp/c877.dis  cvx8897_tables.json
python3 xcheck_cvx8897_win.py /tmp/win.dis "…/avmu3_x64.sys" cvx8897_tables.json

# 3. emit a C++ header for embedding
python3 gen_cvx8897_header.py cvx8897_tables.json > ../../src/liblgx/liblgx-common/src/cvx8897/cvx8897_tables.h
```

## Modes

| Mode key | Input → output |
|---|---|
| `1920_1080_60P_422_To_1920_1080_60P_422` | 1080p60 → 1080p60 (passthrough / bypass) |
| `1920_1080_60P_422_To_1920_1080_30P_422` | 1080p60 → 1080p30 |
| `1920_1080_50P_422_To_1920_1080_25P_422` | 1080p50 → 1080p25 |
| `1280_720_60P_422_To_1280_720_30P_422` | 720p60 → 720p30 |
| `1280_720_50P_422_To_1280_720_25P_422` | 720p50 → 720p25 |
| `1280_720_60P_RGB_To_1280_720_30P_RGB` | 720p60 RGB → 720p30 RGB |
| `1680_1050_60P_422_To_1680_1050_30P_422` | 1680×1050p60 → 30 |
| `1440_900_60P_422_To_1440_900_30P_422` | 1440×900p60 → 30 |
| `1360_768_60P_422_To_1360_768_30P_422` | 1360×768p60 → 30 |
| `1280_1024_60P_422_To_1280_1024_30P_422` | 1280×1024p60 → 30 |
| `1280_800_60P_422_To_1280_800_30P_422` | 1280×800p60 → 30 |
| `1280_768_60P_422_To_1280_768_30P_422` | 1280×768p60 → 30 |
| `1024_768_60P_422_To_1024_768_30P_422` | 1024×768p60 → 30 |
| `800_600_60P_422_To_800_600_30P_422` | 800×600p60 → 30 |

The passthrough table differs from the 1080p60→30 table in only 14 registers:
`0x07, 0x08, 0x0a, 0x10, 0x3a, 0x45, 0x81, 0x8c, 0x98, 0xa5, 0xa6, 0xaa, 0xaf, 0xf0`.

## Vendor API

`CVX8897::SetParam(VX8879_PARAM)` is a jump table over 14 `REG_*` methods and is
the entry point the application uses to select a converter mode.
`SetMode(VX8897_MODE)` handles power / reset / output-enable,
`SetAllOutputMode(VX8879_OUTPUT_MODE)` writes registers `0x8a`/`0x0d`, and
`HardwareInit()` is empty (the FX3 firmware leaves the converter in a usable
state).
