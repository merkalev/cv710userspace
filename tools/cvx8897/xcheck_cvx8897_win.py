#!/usr/bin/env python3
"""Cross-check the mac-extracted CVX8897 tables against avmu3_x64.sys.

The Windows x64 build writes each register with the sequence

    movb $reg, %dl
    movb $val, %r8b
    movq this, %rcx
    callq CVX8897::WriteRegister

so we can recover the same (reg, val) stream and compare it to the JSON produced
by extract_cvx8897.py.  The two builds share the same code and format strings, so
this is a genuine independent validation (`14 matched, 0 mismatched`).

Usage:
    llvm-objdump -d --arch=x86-64 avmu3_x64.sys > win.dis
    python3 xcheck_cvx8897_win.py win.dis avmu3_x64.sys cvx8897_tables.json
"""
import argparse
import json
import re

line_re = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*?))?\s*$')
TEXT_VA, TEXT_OFF = 0x140001000, 0x400
WRITE_REG = "0x1400966a4"


def va_of(off):
    return TEXT_VA + (off - TEXT_OFF)


def str_at(data, va, n=48):
    off = va - TEXT_VA + TEXT_OFF
    return data[off:off + n].split(b'\x00')[0].decode('latin1')


def imm(tok):
    m = re.match(r'\$(-?0x[0-9a-f]+|-?\d+)', tok)
    if not m:
        return None
    return int(m.group(1), 16 if 'x' in m.group(1) else 10) & 0xff


def parse_win(path):
    ins = []
    for ln in open(path, errors="replace"):
        m = line_re.match(ln)
        if m:
            ins.append((int(m.group(1), 16), m.group(2), (m.group(3) or "").strip()))
    return ins


def extract(ins, lo, hi):
    writes, reg, val = [], None, None
    for addr, mnem, ops in ins:
        if addr < lo:
            continue
        if addr >= hi:
            break
        if mnem in ("movb", "movl") and (ops.endswith("%dl") or ops.endswith("%edx")):
            v = imm(ops)
            reg = v if v is not None else reg
        elif mnem in ("movb", "movl") and (ops.endswith("%r8b") or ops.endswith("%r8d")):
            v = imm(ops)
            val = v if v is not None else val
        elif mnem in ("xorl", "xorb") and ops.replace(" ", "") in ("%r8d,%r8d", "%r8b,%r8b"):
            val = 0
        elif mnem == "callq":
            if ops.startswith(WRITE_REG):
                writes.append((reg, val))
            reg = val = None
    return writes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dis")
    ap.add_argument("sys")
    ap.add_argument("json", nargs="?", default="cvx8897_tables.json")
    args = ap.parse_args()

    data = open(args.sys, "rb").read()
    ins = parse_win(args.dis)
    idx = {a: i for i, (a, _, _) in enumerate(ins)}
    tables = json.load(open(args.json))
    names = list(tables["modes"])

    ok = bad = 0
    for n in names:
        needle = ("CVX8897::REG_" + n).encode()
        off = data.find(needle)
        if off < 0:
            print(f"{n}: name not found")
            bad += 1
            continue
        v = va_of(off)
        xrefs = [a for a, m, o in ins if m == "leaq" and f"# {v:#x}" in o]
        entry = exit_ = None
        for xa in xrefs:
            i = idx[xa]
            for j in range(i + 1, min(i + 5, len(ins))):
                if ins[j][1] == "leaq" and "%rdx" in ins[j][2]:
                    m2 = re.search(r"# (0x[0-9a-f]+)", ins[j][2])
                    if m2:
                        s = str_at(data, int(m2.group(1), 16))
                        if s.startswith("%s  +"):
                            entry = xa
                        elif s.startswith("%s  -"):
                            exit_ = xa
                    break
        if entry is None or exit_ is None:
            print(f"{n}: entry={entry} exit={exit_}")
            bad += 1
            continue
        w = extract(ins, entry, exit_ + 1)
        mw = list(zip(tables["reg_order"], tables["modes"][n]))
        match = (w == mw)
        print(f"{n}: win={len(w)} mac={len(mw)} match={match}")
        ok += match
        bad += (not match)
    print(f"\n{ok} matched, {bad} mismatched")
    raise SystemExit(1 if bad else 0)


if __name__ == "__main__":
    main()
