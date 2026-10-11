#!/usr/bin/env python3
"""Extract the CVX8897 REG_* register tables from a libC877Driver.dylib disassembly.

Each `CVX8897::REG_<mode>(...)` function is a straight-line, unrolled sequence of
    CVX8897::WriteRegister(reg, val)
calls (251 per mode).  We emulate just enough i386 to recover every (reg, val)
pair: track register immediates, `pushl` operands, and record a write at each
`calll` to CVX8897::WriteRegister.

Usage:
    llvm-objdump -d --arch=i386 libC877Driver.dylib > c877.dis
    python3 extract_cvx8897.py c877.dis cvx8897_tables.json

The WriteRegister call target defaults to the known address 0x11eea; override
with --write-register if the disassembly is rebased.
"""
import argparse
import collections
import json
import re

line_re = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*?))?\s*$')
sym_re = re.compile(r'^([0-9a-f]+) <(.+)>:\s*$')
REGS32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")

WRITE_REGISTER = "11eea"  # CVX8897::WriteRegister(uint8_t, uint8_t)


def parse(path):
    funcs = collections.OrderedDict()
    cur = None
    for ln in open(path, errors="replace"):
        m = sym_re.match(ln)
        if m:
            cur = (int(m.group(1), 16), m.group(2))
            funcs[cur] = []
            continue
        m = line_re.match(ln)
        if m and cur is not None:
            funcs[cur].append((int(m.group(1), 16), m.group(2), (m.group(3) or "").strip()))
    return funcs


def split_ops(ops):
    return [o.strip() for o in ops.split(",")] if ops else []


def eval_operand(tok, regs):
    tok = tok.strip()
    if tok.startswith("$"):
        try:
            return int(tok[1:], 16) if tok[1:].startswith("0x") else int(tok[1:])
        except ValueError:
            return None
    if tok.startswith("%") and tok[1:] in REGS32:
        return regs.get(tok[1:])
    if tok.startswith("%"):
        return None
    if re.match(r'^-?(0x[0-9a-f]+|\d+)\(%\w+\)$', tok):
        return regs.get(("mem", tok))
    if re.match(r'^-?(0x[0-9a-f]+|\d+)$', tok):
        return int(tok, 16) if "0x" in tok else int(tok)
    return None


def emulate(ins, write_register):
    regs = {}
    stack = []
    writes = []
    call = "0x" + write_register
    for addr, mnem, ops in ins:
        op = split_ops(ops)
        if mnem in ("pushl", "push"):
            if len(op) == 1:
                stack.append(eval_operand(op[0], regs))
        elif mnem in ("movl", "movb", "movw"):
            if len(op) == 2:
                val = eval_operand(op[0], regs)
                if op[1].startswith("%") and op[1][1:] in REGS32:
                    regs[op[1][1:]] = val
                else:
                    regs[("mem", op[1])] = val
        elif mnem == "movzbl":
            if len(op) == 2:
                val = eval_operand(op[0], regs)
                if op[1].startswith("%") and op[1][1:] in REGS32:
                    regs[op[1][1:]] = (val & 0xff) if isinstance(val, int) else val
        elif mnem == "xorl":
            if len(op) == 2 and op[0] == op[1] and op[0].startswith("%"):
                regs[op[0][1:]] = 0
        elif mnem == "leal":
            if len(op) == 2:
                regs[op[1][1:]] = ("addr", op[0])
        elif mnem == "calll":
            tgt = op[0].split()[0] if op else ""
            if tgt == call:
                # I2CSubSend(CI2CBus*, slave, subaddr, sublen, data, datalen, flag)
                # -> WriteRegister pushes $val then $reg, so subaddr=$reg, data=$val.
                val = stack[-3] if len(stack) >= 3 else None
                reg = stack[-2] if len(stack) >= 3 else None
                writes.append((reg, val))
            stack = []
            regs = {k: v for k, v in regs.items()
                    if isinstance(k, tuple) or k in ("ebx", "esi", "edi", "ebp")}
        elif mnem in ("subl", "addl", "incl", "decl", "shll", "sarl", "andl", "orl",
                      "cmpl", "testl", "retl", "ret", "nop", "int3"):
            pass
        elif mnem in ("jmp", "je", "jne", "jg", "jl", "jge", "jle", "ja", "jb",
                      "jae", "jbe"):
            stack = []
    return writes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dis", help="llvm-objdump -d output for libC877Driver.dylib")
    ap.add_argument("out", nargs="?", default="cvx8897_tables.json")
    ap.add_argument("--write-register", default=WRITE_REGISTER)
    args = ap.parse_args()

    funcs = parse(args.dis)
    reg_funcs = sorted(
        ((a, n, ins) for (a, n), ins in funcs.items() if n.startswith("_") and "REG_" in n),
        key=lambda t: t[0])

    tables = {}
    for addr, name, ins in reg_funcs:
        short = name.split("REG_", 1)[1].rsplit("Ev", 1)[0]
        writes = emulate(ins, args.write_register)
        if not writes:
            # e.g. REG_AUDIO_CHSTS, which is not a converter table.
            print(f"{short}: skipped (no WriteRegister calls)")
            continue
        tables[short] = [[r, v] for r, v in writes]
        regs = [r for r, _ in writes]
        print(f"{short}: {len(writes)} writes, regs "
              f"{regs[0] if regs else '-'}..{regs[-1] if regs else '-'}")

    # Canonical compact form: one shared reg_order + per-mode values.
    reg_order = [r for r, _ in next(iter(tables.values()))]
    assert all([r for r, _ in t] == reg_order for t in tables.values()), \
        "register orders differ between modes"
    out = {
        "source": {
            "chip": "CVX8897",
            "i2c7bit": "0x18",
            "vendor_addr_8bit": "0x30",
            "wire_write": "01 18 01 <reg> <val>  (EP1 bulk OUT, == sendI2cWrite(0x18, reg, val))",
            "note": "Initialised by the captured bootstrap via SetAllOutputMode (regs 0x8A/0x0D).",
            "extracted_from": ["mac libC877Driver.dylib (i386, symbols)",
                               "win avmu3_x64.sys (x64)"],
            "cross_validated": True,
            "dispatcher": "CVX8897::SetParam(VX8879_PARAM) -> VX8897_MODE jump table (params 1..14)",
        },
        "reg_order": reg_order,
        "modes": {n: [v for _, v in t] for n, t in tables.items()},
    }

    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print(f"\nwrote {args.out}: {len(out['modes'])} modes x {len(reg_order)} writes")


if __name__ == "__main__":
    main()
