#!/usr/bin/env python3
"""
gen_writ_diagnostics.py - Map writ's diagnostic codes into n00b's registry.

Reads writ/include/writ/diag_codes.def and writes compiler/diagnostics/writ.def, which
gives every writ code that can reach a n00b user its own N00B-F1xxx code.

Usage:
    python3 tools/gen_writ_diagnostics.py WRIT_DEF OUTPUT [--check]
"""

import re
import sys

FAMILIES = {
    "00": "The contract inside an 'ffi literal (or a .writ file it names) could not\n"
          "be read as written. Fix the contract at the location shown.",
    "01": "A declaration in the 'ffi contract is invalid or refers to something\n"
          "the contract does not declare. Fix the declaration at the location shown.",
    "02": "A type, parameter or clause in the 'ffi contract does not describe the\n"
          "C interface consistently. Fix the clause at the location shown.",
    "03": "The contract was checked against the C compiler and the library, and\n"
          "they disagree. The message says which part; fix the contract or the\n"
          "build setup.",
    "05": "The `binding` block of an 'ffi literal chooses how the contract appears\n"
          "in n00b. The choice at the location shown is not valid for this\n"
          "contract.",
    "09": "This 'ffi contract feature is not supported yet.",
}

INTERNAL = {"E0004": "the contract grammar produced more than one parse"}

INTERNAL_EXPLANATION = (
    "This is a bug in the contract grammar, not in your program. Please\n"
    "report it together with the contract that reproduces it.")

SEVERITIES = {"WRIT_ERROR": "ERROR", "WRIT_WARNING": "WARNING",
              "WRIT_NOTE": "NOTE"}

ENTRY = re.compile(r'WRIT_DIAG\((\w+), "([EN])(\d{4})", (\w+), "(.*)"\)\s*$')


def c_string_lines(text):
    parts = text.replace("\\", "\\\\").replace('"', '\\"').split("\n")
    return "\n".join(f'    "{part}\\n"' if i + 1 < len(parts)
                     else f'    "{part}"' for i, part in enumerate(parts))


def generate(source):
    out = []
    for line in source.splitlines():
        match = ENTRY.match(line)
        if not match:
            continue
        ident, letter, number, severity, title = match.groups()
        internal = letter + number in INTERNAL
        out.append(f'N00B_WRIT_DIAG({ident}, "N00B-F1{number[1:]}", '
                   f'{"INTERNAL" if internal else SEVERITIES[severity]},\n'
                   f'    "{INTERNAL[letter + number] if internal else title}",\n'
                   f'{c_string_lines(INTERNAL_EXPLANATION if internal else FAMILIES[number[:2]])})')
    return "\n".join(out) + "\n"


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    if len(args) != 2:
        print("gen_writ_diagnostics.py: error: expected WRIT_DEF and OUTPUT",
              file=sys.stderr)
        return 2
    text = generate(open(args[0], encoding="utf-8").read())
    if "--check" in sys.argv:
        current = open(args[1], encoding="utf-8").read()
        if current != text:
            print(f"gen_writ_diagnostics.py: error: {args[1]} is out of date; "
                  "regenerate it", file=sys.stderr)
            return 1
        return 0
    open(args[1], "w", encoding="utf-8").write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
