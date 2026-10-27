#!/usr/bin/env python3
"""
gen_lantern_unicode.py - Generate lantern's C Unicode tables from the UCD.

Writes lantern/src/unicode/ucd_tables.c: XID_Start, XID_Continue,
Extended_Pictographic, wide and zero-width display ranges, Script with its
names, canonical combining class, decomposition mappings, primary
compositions, and confusable prototypes.
Inputs come from the same pinned, checksummed cache as gen_unicode_tables.py.

Usage:
    python3 tools/gen_lantern_unicode.py [--cache-dir DIR] [--allow-downloads]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_unicode_tables as ucd

INPUTS = {
    "UnicodeData.txt": "UnicodeData.txt",
    "DerivedCoreProperties.txt": "DerivedCoreProperties.txt",
    "DerivedNormalizationProps.txt": "DerivedNormalizationProps.txt",
    "Scripts.txt": "Scripts.txt",
    "PropertyValueAliases.txt": "PropertyValueAliases.txt",
    "emoji_emoji-data.txt": "emoji/emoji-data.txt",
    "EastAsianWidth.txt": "EastAsianWidth.txt",
}


def fetch(versions, cache, allow):
    base = ucd.UNICODE_BASE.format(version=versions["ucd"])
    for name, remote in INPUTS.items():
        ucd.download_file(f"{base}/{remote}", cache / name, allow,
                          cache.key(name))
        ucd.require_nonempty_file(cache / name, "Unicode cache file")
    standard, template = ucd.SPECIAL_FILES["confusables.txt"]
    ucd.download_file(template.format(version=versions[standard]),
                      cache / "confusables.txt", allow,
                      cache.key("confusables.txt"))
    ucd.require_nonempty_file(cache / "confusables.txt", "Unicode cache file")


def property_ranges(path, prop):
    points = set()
    for (start, end), fields in ucd.parse_semicolon_file(path):
        if fields and fields[0] == prop:
            points.update(range(start, end + 1))
    return merge(sorted(points))


def merge(points):
    ranges = []
    for cp in points:
        if ranges and ranges[-1][1] + 1 == cp:
            ranges[-1][1] = cp
        else:
            ranges.append([cp, cp])
    return ranges


def valued_ranges(values):
    ranges = []
    for cp in sorted(values):
        value = values[cp]
        if ranges and ranges[-1][1] + 1 == cp and ranges[-1][2] == value:
            ranges[-1][1] = cp
        else:
            ranges.append([cp, cp, value])
    return ranges


def scripts(cache):
    aliases = []
    for line in open(cache / "PropertyValueAliases.txt", encoding="utf-8"):
        parts = [x.strip() for x in line.split("#", 1)[0].split(";")]
        if len(parts) >= 3 and parts[0] == "sc":
            aliases.append((parts[2], parts[1], parts[3:]))
    fixed = ["Unknown", "Common", "Inherited"]
    by_long = {long: (short, extra) for long, short, extra in aliases}
    order = fixed + sorted(n for n in by_long if n not in fixed)
    index = {name: i for i, name in enumerate(order)}
    values = {}
    for (start, end), fields in ucd.parse_semicolon_file(cache / "Scripts.txt"):
        for cp in range(start, end + 1):
            values[cp] = index[fields[0]]
    names = [(name, by_long[name][0]) for name in order]
    return names, valued_ranges(values)


def wide_ranges(cache):
    path = cache / "EastAsianWidth.txt"
    points = set()
    for (start, end), fields in (ucd.parse_missing_defaults(path)
                                 + list(ucd.parse_semicolon_file(path))):
        if fields and fields[0] in ("W", "F"):
            points.update(range(start, end + 1))
        elif fields:
            points.difference_update(range(start, end + 1))
    return merge(sorted(points))


def zero_width_ranges(cache):
    points = set(range(0x1160, 0x1200)) | {0x200B}
    for cp, fields in ucd.parse_unicode_data(cache / "UnicodeData.txt").items():
        if fields[2] in ("Mn", "Me", "Cf"):
            points.add(cp)
    return merge(sorted(points))


def unicode_data(cache):
    ccc = {}
    mappings = {}
    for cp, fields in ucd.parse_unicode_data(cache / "UnicodeData.txt").items():
        if int(fields[3]):
            ccc[cp] = int(fields[3])
        decomposition = fields[5].strip()
        if not decomposition:
            continue
        compat = decomposition.startswith("<")
        if compat:
            decomposition = decomposition.split(">", 1)[1].strip()
        mappings[cp] = (compat, [int(x, 16) for x in decomposition.split()])
    return ccc, mappings


def compositions(cache, mappings):
    excluded = set()
    for (start, end), fields in ucd.parse_semicolon_file(
            cache / "DerivedNormalizationProps.txt"):
        if fields and fields[0] == "Full_Composition_Exclusion":
            excluded.update(range(start, end + 1))
    pairs = []
    for cp, (compat, target) in mappings.items():
        if not compat and len(target) == 2 and cp not in excluded:
            pairs.append((target[0], target[1], cp))
    return sorted(pairs)


def confusables(cache):
    result = {}
    for line in open(cache / "confusables.txt", encoding="utf-8-sig"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(";")]
        source = [int(x, 16) for x in parts[0].split()]
        if len(source) == 1:
            result[source[0]] = [int(x, 16) for x in parts[1].split()]
    return result


def emit_ranges(out, name, ranges):
    out.append(f"const ucd_range_t {name}[] = {{")
    for first, last in ranges:
        out.append(f"    {{0x{first:X}, 0x{last:X}}},")
    out.append("};")
    out.append(f"const size_t {name}_count = {len(ranges)};\n")


def emit_valued(out, name, ranges):
    out.append(f"const ucd_valued_range_t {name}[] = {{")
    for first, last, value in ranges:
        out.append(f"    {{0x{first:X}, 0x{last:X}, {value}}},")
    out.append("};")
    out.append(f"const size_t {name}_count = {len(ranges)};\n")


def emit_mappings(out, name, mappings, compat_flag):
    pool = []
    rows = []
    for cp in sorted(mappings):
        compat, target = compat_flag(mappings[cp])
        rows.append((cp, len(pool), len(target), compat))
        pool.extend(target)
    out.append(f"const uint32_t {name}_pool[] = {{")
    for i in range(0, len(pool), 8):
        out.append("    " + " ".join(f"0x{cp:X}," for cp in pool[i:i + 8]))
    out.append("};")
    out.append(f"const ucd_mapping_t {name}[] = {{")
    for cp, offset, length, compat in rows:
        out.append(f"    {{0x{cp:X}, {offset}, {length}, "
                   f"{'true' if compat else 'false'}}},")
    out.append("};")
    out.append(f"const size_t {name}_count = {len(rows)};\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--ucd-version", default="18.0.0")
    parser.add_argument("--cache-dir", default=str(Path.home() / ".cache"
                                                   / "n00b" / "ucd"))
    parser.add_argument("--allow-downloads", action="store_true")
    parser.add_argument("--output", default="lantern/src/unicode/ucd_tables.c")
    args = parser.parse_args()

    versions = {"ucd": args.ucd_version, "security": args.ucd_version}
    cache = ucd.UnicodeCache(Path(args.cache_dir).resolve(), versions)
    for directory in cache.dirs.values():
        directory.mkdir(parents=True, exist_ok=True)
    fetch(versions, cache, args.allow_downloads)

    derived = cache / "DerivedCoreProperties.txt"
    names, script_ranges = scripts(cache)
    ccc, mappings = unicode_data(cache)

    out = [
        '#include "private/unicode/ucd_tables.h"',
        "",
        f'const core_string_t ucd_unicode_version = '
        f'CORE_STRING_STATIC("{args.ucd_version}");\n',
    ]
    emit_ranges(out, "ucd_xid_start", property_ranges(derived, "XID_Start"))
    emit_ranges(out, "ucd_xid_continue",
                property_ranges(derived, "XID_Continue"))
    emit_ranges(out, "ucd_wide", wide_ranges(cache))
    emit_ranges(out, "ucd_zero_width", zero_width_ranges(cache))
    emit_ranges(out, "ucd_extended_pictographic",
                property_ranges(cache / "emoji_emoji-data.txt",
                                "Extended_Pictographic"))
    out.append("const ucd_script_name_t ucd_script_names[] = {")
    for long, short in names:
        out.append(f'    {{CORE_STRING_STATIC("{long}"), '
                   f'CORE_STRING_STATIC("{short}")}},')
    out.append("};")
    out.append(f"const size_t ucd_script_names_count = {len(names)};\n")
    emit_valued(out, "ucd_scripts", script_ranges)
    emit_valued(out, "ucd_combining_class", valued_ranges(ccc))
    emit_mappings(out, "ucd_decompositions", mappings, lambda m: m)
    out.append("const ucd_composition_t ucd_compositions[] = {")
    pairs = compositions(cache, mappings)
    for first, second, composite in pairs:
        out.append(f"    {{0x{first:X}, 0x{second:X}, 0x{composite:X}}},")
    out.append("};")
    out.append(f"const size_t ucd_compositions_count = {len(pairs)};\n")
    emit_mappings(out, "ucd_confusables", confusables(cache),
                  lambda target: (False, target))

    Path(args.output).write_text("\n".join(out))
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
