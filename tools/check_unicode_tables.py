#!/usr/bin/env python3

import argparse
import os
import re
import struct
import sys
from pathlib import Path

SKIP = 77

C_TYPES = {"uint8_t": "u8", "uint16_t": "u16", "uint32_t": "u32",
           "uint64_t": "u64", "string_t": "string"}

NUMBER = re.compile(r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*")
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
C_DEFINE = re.compile(r"^%define\s+N00B_UNICODE_(\w+)\s+(\S+)\s*$", re.M)
C_DECL = re.compile(
    r"export\s+(?:const\s+)?(\w+)\s+unicode_(\w+)((?:\[\d*\])*)\s*=\s*"
    r"(\{.*?\};|[^;{]+;)", re.S)
C_VERSION = re.compile(
    r"export\s+string_t\s+unicode_version\s*\(void\)\s*\{\s*return\s+"
    r'"([^"]*)"\s*;\s*\}', re.S)
C_EMBED = re.compile(r'%embed\s+"([^"]+)"')
N_CONST = re.compile(r"^public const (\w+)(?:\s*:\s*([^=]+?))?\s*=\s*", re.M)
N_VERSION = re.compile(
    r'^public func version\(\) -> string \{\s*return "([^"]*)"\s*\}', re.M)
N_DATA_FILE = re.compile(
    r'sys\.fs\.data_file\(source_dir\(\) \+ "([^"]+)"\)')
N_ACCESSOR = re.compile(
    r"^public (?:inline )?func (\w+)\([^)]*\) -> \w+ \{\n"
    r"    return internal\.blob\.(u16|u32|u64|str)_at\((?:ref )?(\w+)_bytes, "
    r"(?:row \* (\d+) \+ col|i)\)\n\}", re.M)
BLOB_CODES = {"u16": "H", "u32": "I", "u64": "Q"}


def number(text):
    digits = NUMBER.fullmatch(text.strip())
    if digits is None:
        raise ValueError(f"not a number: {text!r}")
    raw = digits.group(1)
    return int(raw, 16) if raw[:2] in ("0x", "0X") else int(raw, 10)


def numbers(text):
    return [number(m.group(0)) for m in NUMBER.finditer(text)]


def parse_c(path, baseline):
    text = re.sub(r"//[^\n]*", "", path.read_text())
    tables = {}
    for name, value in C_DEFINE.findall(text):
        tables[name.lower()] = (None, number(value))
    version = C_VERSION.search(text)
    if version:
        tables["version"] = ("func string", version.group(1))
    for c_type, name, dims, body in C_DECL.findall(text):
        base = C_TYPES[c_type]
        depth = dims.count("[")
        n00b_type = base
        for _ in range(depth):
            n00b_type = f"Array[{n00b_type}]"
        if depth == 0:
            tables[name] = (n00b_type, number(body.rstrip(";")))
            continue
        inner = body.strip()[1:-2]
        embed = C_EMBED.search(inner)
        if embed:
            blob = baseline / Path(embed.group(1)).name
            tables[name] = (n00b_type, list(blob.read_bytes()))
        elif base == "string":
            tables[name] = (n00b_type, STRING.findall(inner))
        elif depth == 2:
            rows = re.findall(r"\{([^{}]*)\}", inner)
            tables[name] = (n00b_type, [numbers(r) for r in rows])
        else:
            tables[name] = (n00b_type, numbers(inner))
    return tables


def bracket_end(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unterminated array literal")


def decode_blob(data, elem, width):
    if elem == "str":
        count = struct.unpack_from("<I", data)[0] // 4
        offsets = struct.unpack_from(f"<{count}I", data)
        return ("Array[string]",
                [data[offsets[i]:offsets[i + 1]].decode("utf-8")
                 for i in range(count - 1)])
    size = struct.calcsize(BLOB_CODES[elem])
    values = list(struct.unpack(f"<{len(data) // size}{BLOB_CODES[elem]}",
                                data))
    if width is None:
        return f"Array[{elem}]", values
    width = int(width)
    return (f"Array[Array[{elem}]]",
            [values[i:i + width] for i in range(0, len(values), width)])


def decode_accessors(path, text, tables, referenced):
    for m in N_ACCESSOR.finditer(text):
        name, elem, blob_name, width = m.groups()
        key = f"{name}_bytes"
        if blob_name != name or key not in tables:
            continue
        del tables[key]
        source = N_DATA_FILE.search(
            text[text.index(f"public const {key}:"):]).group(1)
        blob = path.parent / source.lstrip("/")
        referenced.add(blob.resolve())
        tables[name] = decode_blob(blob.read_bytes(), elem, width)


def parse_n00b(path, referenced):
    text = path.read_text()
    tables = {}
    version = N_VERSION.search(text)
    if version:
        tables["version"] = ("func string", version.group(1))
    for m in N_CONST.finditer(text):
        name = m.group(1)
        n00b_type = re.sub(r"\s+", "", m.group(2)) if m.group(2) else None
        start = m.end()
        if text[start] == "[":
            inner = text[start + 1:bracket_end(text, start)]
            if n00b_type == "Array[string]":
                value = STRING.findall(inner)
            elif n00b_type and n00b_type.startswith("Array[Array["):
                value = [numbers(r) for r in re.findall(r"\[([^\[\]]*)\]",
                                                        inner)]
            else:
                value = numbers(inner)
        else:
            line = text[start:text.index("\n", start)]
            data_file = N_DATA_FILE.fullmatch(line.strip())
            if data_file:
                blob = path.parent / data_file.group(1).lstrip("/")
                value = (list(blob.read_bytes()) if blob.is_file()
                         else f"missing data file {blob}")
            else:
                value = number(line)
        tables[name] = (n00b_type, value)
    decode_accessors(path, text, tables, referenced)
    return tables


def describe(value):
    if isinstance(value, list):
        return f"{len(value)} elements"
    return repr(value)


def compare_tables(label, ours, theirs, errors):
    for name in sorted(set(ours) | set(theirs)):
        if name not in ours:
            errors.append(f"{label}: missing {name}")
            continue
        if name not in theirs:
            errors.append(f"{label}: unexpected {name}")
            continue
        our_type, our_value = ours[name]
        their_type, their_value = theirs[name]
        if our_type != their_type:
            errors.append(f"{label}: {name} has type {our_type}, "
                          f"baseline {their_type}")
        if our_value != their_value:
            detail = ""
            if isinstance(our_value, list) and isinstance(their_value, list):
                for i, (a, b) in enumerate(zip(our_value, their_value)):
                    if a != b:
                        detail = f" (first difference at [{i}]: {a!r} vs {b!r})"
                        break
            errors.append(f"{label}: {name} differs: {describe(our_value)} "
                          f"vs baseline {describe(their_value)}{detail}")


def main():
    parser = argparse.ArgumentParser(description="Check Unicode tables")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent),
                        help="n00b source root")
    parser.add_argument("--baseline",
                        default=os.environ.get(
                            "UNICODE_TABLES_BASELINE",
                            str(Path.home() / "libn00b-str-fixes" / "build-gen")),
                        help="libn00b generated C baseline directory")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    baseline = Path(args.baseline).expanduser()

    if not baseline.is_dir():
        print(f"SKIP: baseline directory absent: {baseline}")
        return SKIP

    generated = root / "lib" / "unicode" / "generated"
    conformance_dir = root / "tests" / "data" / "unicode"
    conformance_program = (root / "tests" / "programs" / "unicode"
                           / "gen_conformance.n")

    errors = []
    referenced = set()
    modules = {f"gen_{p.stem}.c": p for p in generated.glob("*.n")}
    modules["gen_conformance.c"] = conformance_program
    baseline_modules = {p.name: p for p in baseline.glob("gen_*.c")}

    for name in sorted(set(modules) | set(baseline_modules)):
        if name not in modules or not modules[name].is_file():
            errors.append(f"missing committed module for baseline {name}")
        elif name not in baseline_modules:
            errors.append(f"committed module {modules[name]} has no baseline")
        else:
            compare_tables(str(modules[name].relative_to(root)),
                           parse_n00b(modules[name], referenced),
                           parse_c(baseline_modules[name], baseline),
                           errors)

    bins = {f"gen_{p.name}": p for p in generated.glob("*.bin")}
    bins.update({p.name: p for p in conformance_dir.glob("gen_conf_*.bin")})
    baseline_bins = {p.name: p for p in baseline.glob("gen_*.bin")}

    for name in sorted(set(bins) | set(baseline_bins)):
        if name not in bins:
            errors.append(f"missing committed blob for baseline {name}")
        elif name not in baseline_bins:
            if bins[name].resolve() not in referenced:
                errors.append(f"committed blob {bins[name]} has no baseline "
                              f"and no table accessor")
        elif bins[name].read_bytes() != baseline_bins[name].read_bytes():
            errors.append(f"blob {bins[name]} differs from baseline {name}")

    for line in errors:
        print(f"FAIL {line}")
    if errors:
        return 1
    print(f"ok: {len(bins)} blobs and {len(modules)} modules match {baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
