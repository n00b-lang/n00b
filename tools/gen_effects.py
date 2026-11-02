#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

DEFINITIONS = "lantern/include/ir/summary_effects.def"
OUTPUT = "lib/sys/internal/effects.n"
ROW = re.compile(r"^IR_EFFECT_(FLAG|TRAP|NONLOCAL|PARAMETER)\(\s*(\w+)\s*,"
                 r"\s*(\w+)\s*,\s*(\d+)\s*\)", re.M)


def rows(root):
    text = (root / DEFINITIONS).read_text()
    return [(kind, name, int(bit)) for kind, _, name, bit in ROW.findall(text)]


def render(entries):
    flags = [(name, bit) for kind, name, bit in entries if kind == "FLAG"]
    trap_shift = max(bit for _, bit in flags) + 1
    constants = [(name, 1 << bit) for name, bit in flags]
    constants += [("traps_" + name, 1 << (trap_shift + bit))
                  for kind, name, bit in entries if kind == "TRAP"]
    constants += [(name, 1 << bit)
                  for kind, name, bit in entries if kind == "NONLOCAL"]
    width = max(len(name) for name, _ in constants) + 1
    lines = ["const %s u64 = 0x%x" % ((name + ":").ljust(width), value)
             for name, value in constants]
    for kind, name, shift in entries:
        if kind != "PARAMETER":
            continue
        lines += ["", "func %s(index: u64) -> u64 {" % name,
                  "    return cast[u64](1) << (%d + index)" % shift, "}"]
    lines += ["", "func has_effect(effects: u64, effect: u64) -> bool {",
              "    return effects & effect != 0", "}"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent,
                        type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render(rows(args.root))
    target = args.root / OUTPUT
    if args.check:
        if target.read_text() != text:
            print("%s is stale; run make regen-effects" % OUTPUT,
                  file=sys.stderr)
            return 1
        return 0
    target.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
