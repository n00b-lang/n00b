#!/usr/bin/env python3
"""
gen_unicode_tables.py — Download Unicode Character Database files and
generate optimized two-stage lookup tables as n00b source for the unicode
package.

Each standard is pinned separately because they can publish on their own
schedules. As of Unicode 18.0.0 the UCD, UCA, security, IDNA and emoji
data are synchronized and colocated under Public/18.0.0/, so every pin
defaults to 18.0.0. Mixing pins across major versions is not supported
and will produce tables that disagree with each other.

Usage:
    python3 tools/gen_unicode_tables.py [--ucd-version 18.0.0]
                                [--uca-version 18.0.0]
                                [--security-version 18.0.0]
                                [--idna-version 18.0.0]
                                [--cache-dir .unicode_cache]
                                [--allow-downloads] [--strict|--no-strict]
                                [--check-inputs]
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TABLE_PROLOGUE = ""

DATA_FILE_IMPORT = "import sys.fs\n\n"

N00B_TYPES = {"uint8_t": "u8", "uint16_t": "u16", "uint32_t": "u32",
              "uint64_t": "u64", "int8_t": "i8"}


BLOCK_SIZE = 256  # Stage-2 block size (codepoints per block)
MAX_CP = 0x110000  # U+0000..U+10FFFF
NUM_BLOCKS = MAX_CP // BLOCK_SIZE  # 4352 stage-1 entries

UNICODE_BASE = "https://www.unicode.org/Public/{version}/ucd"

# Files to download relative to the UCD base
UNICODE_FILES = [
    "UnicodeData.txt",
    "PropList.txt",
    "DerivedCoreProperties.txt",
    "DerivedNormalizationProps.txt",
    "Scripts.txt",
    "ScriptExtensions.txt",
    "Blocks.txt",
    "DerivedAge.txt",
    "BidiBrackets.txt",
    "CaseFolding.txt",
    "SpecialCasing.txt",
    "auxiliary/GraphemeBreakProperty.txt",
    "auxiliary/WordBreakProperty.txt",
    "auxiliary/SentenceBreakProperty.txt",
    "LineBreak.txt",
    "EastAsianWidth.txt",
    "ArabicShaping.txt",
    "emoji/emoji-data.txt",
    "PropertyValueAliases.txt",
    "extracted/DerivedBidiClass.txt",
]

# Test data files
TEST_FILES = [
    "NormalizationTest.txt",
    "auxiliary/GraphemeBreakTest.txt",
    "auxiliary/WordBreakTest.txt",
    "auxiliary/SentenceBreakTest.txt",
    "auxiliary/LineBreakTest.txt",
    "BidiCharacterTest.txt",
]

# Additional files with special URL patterns
SPECIAL_FILES = {
    # version-specific subdirs
    "allkeys.txt":       ("uca", "https://www.unicode.org/Public/{version}/uca/allkeys.txt"),
    "CollationTest.zip": ("uca", "https://www.unicode.org/Public/{version}/uca/CollationTest.zip"),
    "confusables.txt":   ("security", "https://www.unicode.org/Public/{version}/security/confusables.txt"),
    "IdentifierStatus.txt": ("security", "https://www.unicode.org/Public/{version}/security/IdentifierStatus.txt"),
    "IdnaMappingTable.txt": ("idna", "https://www.unicode.org/Public/{version}/idna/IdnaMappingTable.txt"),
    "IdnaTestV2.txt":       ("idna", "https://www.unicode.org/Public/{version}/idna/IdnaTestV2.txt"),
    "scriptMetadata.txt": ("cldr", "https://raw.githubusercontent.com/unicode-org/cldr/{version}/common/properties/scriptMetadata.txt"),
}

GENERATED_FILES = [
    "versions.n",
    "age.n",
    "bidi_brackets.n",
    "bidi_class.n",
    "blocks.n",
    "casemap.n",
    "categories.n",
    "collation.n",
    "combining.n",
    "composition.n",
    "confusables.n",
    "decomposition.n",
    "eaw.n",
    "grapheme.n",
    "identifiers.n",
    "idna.n",
    "incb.n",
    "joining.n",
    "linebreak.n",
    "names.n",
    "normprops.n",
    "proplist.n",
    "script_extensions.n",
    "scripts.n",
    "sentbreak.n",
    "wordbreak.n",
    "age_stage2.bin",
    "bidi_class_stage2.bin",
    "categories_stage2.bin",
    "combining_stage2.bin",
    "eaw_stage2.bin",
    "grapheme_stage2.bin",
    "identifiers_stage2.bin",
    "incb_stage2.bin",
    "idna_stage2.bin",
    "joining_stage2.bin",
    "linebreak_stage2.bin",
    "names_by_name.bin",
    "names_index.bin",
    "names_pool.bin",
    "normprops_stage2.bin",
    "scripts_recommended.bin",
    "scripts_stage2.bin",
    "sentbreak_stage2.bin",
    "wordbreak_stage2.bin",
]

GENERATED_CONFORMANCE_FILES = [
    "gen_conf_bidi.bin",
    "gen_conf_collate.bin",
    "gen_conf_grapheme.bin",
    "gen_conf_idna.bin",
    "gen_conf_line.bin",
    "gen_conf_norm.bin",
    "gen_conf_sentence.bin",
    "gen_conf_word.bin",
]

CACHE_STANDARD = {
    **{f.replace("/", "_"): "ucd" for f in UNICODE_FILES + TEST_FILES},
    **{name: standard for name, (standard, _) in SPECIAL_FILES.items()},
}

REQUIRED_CACHE_FILES = list(CACHE_STANDARD.keys())

SHA256 = {
    "ucd-18.0.0/UnicodeData.txt":
        "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
    "ucd-18.0.0/PropList.txt":
        "f438f532e8737bb8a2702126cdf9c4af5e357c58c7acf9d9eb2fc7c1a1d955d6",
    "ucd-18.0.0/DerivedCoreProperties.txt":
        "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
    "ucd-18.0.0/DerivedNormalizationProps.txt":
        "98ac7f67d985fe781e317f6182e885e94cabb0c314769e6dd73e48b226931ccd",
    "ucd-18.0.0/Scripts.txt":
        "0071fd81b6aeae25f6e8bce8efec3066a6476a91b49bdb2f52dc76e817862a6a",
    "ucd-18.0.0/ScriptExtensions.txt":
        "5c9d34a922f687726f2a8bcf57d49f905987e51f1b21b58c95a00fbe255cec23",
    "ucd-18.0.0/Blocks.txt":
        "a58f8d322f3c5e254f9f97b1cbf76a454a7e02de6ac35619a5e4f27aaabfd553",
    "ucd-18.0.0/DerivedAge.txt":
        "58b04334e3a31f9612694bbc3f6bff4a302b7d424565bca9c47e30aa74f8dbac",
    "ucd-18.0.0/BidiBrackets.txt":
        "4b3b62e4a14b84ee752808c810c602534921c09a4a1bf78cfbee566d66c125b3",
    "ucd-18.0.0/CaseFolding.txt":
        "a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d",
    "ucd-18.0.0/SpecialCasing.txt":
        "8538dea57c184f1ef3783885ea79677b10f6efa06423717157e63712f14d1ad2",
    "ucd-18.0.0/auxiliary_GraphemeBreakProperty.txt":
        "0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a",
    "ucd-18.0.0/auxiliary_WordBreakProperty.txt":
        "8dbfa17063e11084201f33c3e76d485d3b9166930c71db8e39ed1c9234171aec",
    "ucd-18.0.0/auxiliary_SentenceBreakProperty.txt":
        "0ac10c9d6dca3b33ac695ff0968f26356465a51626950c463b3dce8fa07e61cc",
    "ucd-18.0.0/LineBreak.txt":
        "ae8cf1970c73f3f1a12d77852df96c4b2b1723ca8b388e36bb5739250c6849de",
    "ucd-18.0.0/EastAsianWidth.txt":
        "a0cf29eacd00cfcaec4381c6b7c281685f18dbb4e7ff82b4076ccb342ca839aa",
    "ucd-18.0.0/ArabicShaping.txt":
        "8ccde4ebd070500a68e8bcb9d5514522fb0bebe57b9ebad3ff925b8e1e669f83",
    "ucd-18.0.0/emoji_emoji-data.txt":
        "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1",
    "ucd-18.0.0/PropertyValueAliases.txt":
        "06c4c8eaf7b0bf34abe73b113da1215bd784ac254d4c223600b90267caa4bbbd",
    "ucd-18.0.0/extracted_DerivedBidiClass.txt":
        "d9e23222522551348ea1ccfbb4f62efbf98982afb95840f8959c08ed992c5607",
    "ucd-18.0.0/NormalizationTest.txt":
        "25a50d816764b04abfb4a646d3eb2b2a803284c3873d9a06757b94fe4513dde3",
    "ucd-18.0.0/auxiliary_GraphemeBreakTest.txt":
        "b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31",
    "ucd-18.0.0/auxiliary_WordBreakTest.txt":
        "3dd70c071781276067c680d87303f60434adce7b067bd063194af374edad86a5",
    "ucd-18.0.0/auxiliary_SentenceBreakTest.txt":
        "84ca7516e7bd1203ab7e8d888ae77f0a0af0a254d10e09a2847000588848885c",
    "ucd-18.0.0/auxiliary_LineBreakTest.txt":
        "fd9ff9eca411080085b41471bba79ae19d9db2e16924f9770b8509f1affd3f95",
    "ucd-18.0.0/BidiCharacterTest.txt":
        "045b24d2c8ab066951bd32fe8c6b4de34647f72b5b1c7df0265f24ab53573e01",
    "uca-18.0.0/allkeys.txt":
        "9b7539f54715e9b9f6686eee87fd846185e689eef7f182ac23fd5eae1eca5814",
    "uca-18.0.0/CollationTest.zip":
        "02af87f32b787fef873eaa9a279420f41c6ea58caa15ed95821e911fc91bf662",
    "security-18.0.0/confusables.txt":
        "6ed3ee967c9dfdf6677d563c9985182fbc50a2efb7d6059cd57b2e2ce18f5b92",
    "security-18.0.0/IdentifierStatus.txt":
        "5863c7d99ca18f213c41c7318aa5528bebfb6d32ec0f1d5944e37192c119aebd",
    "idna-18.0.0/IdnaMappingTable.txt":
        "a03b1eb38032268c696406a83f0972d6a815acd2c8d4151d42ec0fda70ffced1",
    "idna-18.0.0/IdnaTestV2.txt":
        "0236b75c5b20dfd857b3b5cf75509887959d6ab00d6c7bda9b7dc3df518c1fde",
    "cldr-release-48/scriptMetadata.txt":
        "50a0499acddd7a5ce935e8d77b59ab789f91b16c15b95b00236d90137fe662a4",
}

EXPECTED_TEST_DATA_FILES = ([f.split("/")[-1] for f in TEST_FILES]
                            + ["CollationTest.zip", "IdnaTestV2.txt"])
REQUIRED_TEST_DATA_FILES = EXPECTED_TEST_DATA_FILES


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def fail(message):
    raise SystemExit(f"ERROR: {message}")


def require_nonempty_file(path, label):
    if not path.exists():
        fail(f"missing {label}: {path}")
    if not path.is_file():
        fail(f"{label} is not a file: {path}")
    if path.stat().st_size == 0:
        fail(f"empty {label}: {path}")


def is_nonempty_file(path):
    return path.exists() and path.is_file() and path.stat().st_size > 0


class UnicodeCache:
    def __init__(self, root, versions):
        self.root = root
        self.dirs = {standard: root / f"{standard}-{version}"
                     for standard, version in versions.items()}

    def key(self, name):
        return f"{self.dirs[CACHE_STANDARD[name]].name}/{name}"

    def __truediv__(self, name):
        return self.root / self.key(name)


def verify_sha256(key, data):
    expected = SHA256.get(key)
    if expected is None:
        fail(f"no pinned SHA256 for Unicode data file {key}")
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        fail(f"SHA256 mismatch for Unicode data file {key}: "
             f"expected {expected}, got {actual}")


def download_file(url, dest, allow_downloads, key):
    """Ensure file exists locally, optionally downloading when missing."""
    if dest.exists() and not dest.is_file():
        fail(f"Unicode cache path is not a file: {dest}")
    if dest.exists() and dest.stat().st_size > 0:
        verify_sha256(key, dest.read_bytes())
        print(f"  cached: {dest.name}")
        return True

    if not allow_downloads:
        return False

    print(f"  downloading: {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".tmp")
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "Mozilla/5.0 (unicode-gen-tables/1.0)"
        })
        with urllib.request.urlopen(req) as resp:
            data = resp.read()
        if not data:
            print(f"  WARNING: downloaded empty Unicode data file: {url}")
            return False
        verify_sha256(key, data)
        with open(tmp, "wb") as f:
            f.write(data)
        tmp.replace(dest)
        return True
    except Exception as e:
        try:
            tmp.unlink()
        except FileNotFoundError:
            pass
        print(f"  WARNING: failed to download {url}: {e}")
        return False


def copy_cached_file(src, dest):
    """Populate generated test data from the required cache."""
    if dest.exists() and not dest.is_file():
        fail(f"Unicode test data path is not a file: {dest}")
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  cached: {dest.name}")
        return

    require_nonempty_file(src, "Unicode cache file")
    print(f"  copying: {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".tmp")
    shutil.copyfile(src, tmp)
    tmp.replace(dest)


def validate_cache_inputs(cache):
    for name in REQUIRED_CACHE_FILES:
        require_nonempty_file(cache / name, "Unicode cache file")

    if not zipfile.is_zipfile(cache / "CollationTest.zip"):
        fail(f"invalid Unicode collation zip: {cache / 'CollationTest.zip'}")


def validate_test_data_inputs(test_data_dir):
    for name in REQUIRED_TEST_DATA_FILES:
        require_nonempty_file(test_data_dir / name, "Unicode test data file")

    if not zipfile.is_zipfile(test_data_dir / "CollationTest.zip"):
        fail(f"invalid Unicode collation zip: {test_data_dir / 'CollationTest.zip'}")


def validate_generated_outputs(out_dir, conformance_dir, conformance_program,
                               conformance):
    for name in GENERATED_FILES:
        require_nonempty_file(out_dir / name, "generated Unicode table")
    if conformance:
        for name in GENERATED_CONFORMANCE_FILES:
            require_nonempty_file(conformance_dir / name,
                                  "generated Unicode table")
        require_nonempty_file(conformance_program, "generated Unicode table")


def download_all(versions, cache, test_data_dir, allow_downloads):
    """Populate cache/test data and report missing files."""
    version = versions["ucd"]
    base    = UNICODE_BASE.format(version=version)

    if allow_downloads:
        print(f"Downloading UCD {version} files...")
    else:
        print(f"Checking cached UCD {version} files (downloads disabled)...")

    for f in UNICODE_FILES:
        url = f"{base}/{f}"
        name = f.replace("/", "_")
        download_file(url, cache / name, allow_downloads, cache.key(name))

    for f in TEST_FILES:
        url = f"{base}/{f}"
        name = f.replace("/", "_")
        cache_dest = cache / name
        if download_file(url, cache_dest, allow_downloads, cache.key(name)):
            copy_cached_file(cache_dest, test_data_dir / f.split("/")[-1])

    for name, (standard, url_template) in SPECIAL_FILES.items():
        url = url_template.format(version=versions[standard])
        cache_dest = cache / name
        downloaded = download_file(url, cache_dest, allow_downloads,
                                   cache.key(name))
        if name == "CollationTest.zip" or name == "IdnaTestV2.txt":
            if downloaded:
                copy_cached_file(cache_dest, test_data_dir / name)
            else:
                download_file(url, test_data_dir / name, allow_downloads,
                              cache.key(name))

    if allow_downloads:
        print("Download complete.")

    missing_required = [name for name in REQUIRED_CACHE_FILES
                        if not is_nonempty_file(cache / name)]
    missing_test_data = [name for name in EXPECTED_TEST_DATA_FILES
                         if not is_nonempty_file(test_data_dir / name)]
    return missing_required, missing_test_data


def parse_semicolon_file(path, fields=None):
    """Parse a standard UCD semicolon-delimited file.
    Yields (codepoint_or_range, [field_values]).
    codepoint_or_range is (start, end) tuple.
    """
    if not is_nonempty_file(path):
        return
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            cp_range = parse_cp_range(parts[0])
            rest = parts[1:] if len(parts) > 1 else []

            if fields is not None:
                rest = rest[:fields]
            yield cp_range, rest


MISSING_LINE = re.compile(r"^#\s*@missing:\s*(.*)$")


def parse_cp_range(cp_field):
    if ".." in cp_field:
        start, end = cp_field.split("..")
        return (int(start, 16), int(end, 16))
    cp = int(cp_field, 16)
    return (cp, cp)


def parse_missing_defaults(path):
    defaults = []
    if not is_nonempty_file(path):
        return defaults
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = MISSING_LINE.match(line.strip())
            if not m:
                continue
            parts = [p.strip() for p in m.group(1).split("#")[0].split(";")]
            defaults.append((parse_cp_range(parts[0]), parts[1:]))
    return defaults


def property_value_aliases(cache_dir, prop):
    aliases = {}
    for line in open(cache_dir / "PropertyValueAliases.txt",
                     encoding="utf-8"):
        parts = [x.strip() for x in line.split("#", 1)[0].split(";")]
        if len(parts) >= 3 and parts[0] == prop:
            for alias in parts[1:]:
                aliases[alias] = parts[1]
    return aliases


def parse_unicode_data(path):
    """Parse UnicodeData.txt which has a special format with ranges."""
    entries = {}
    if not is_nonempty_file(path):
        return entries
    with open(path, "r", encoding="utf-8") as f:
        prev_range_start = None
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(";")
            cp = int(parts[0], 16)
            name = parts[1]

            if name.endswith(", First>"):
                prev_range_start = cp
                # Store the field data for range fill
                entries[cp] = parts
                continue
            elif name.endswith(", Last>"):
                if prev_range_start is not None:
                    # Fill in the range with the same properties
                    template = entries[prev_range_start]
                    for c in range(prev_range_start, cp + 1):
                        entries[c] = template[:]
                        entries[c][0] = f"{c:04X}"
                    prev_range_start = None
                continue

            entries[cp] = parts

    return entries


# ---------------------------------------------------------------------------
# Enum definitions — must match the C enums exactly
# ---------------------------------------------------------------------------

GC_VALUES = [
    "Lu", "Ll", "Lt", "Lm", "Lo",
    "Mn", "Mc", "Me",
    "Nd", "Nl", "No",
    "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po",
    "Sm", "Sc", "Sk", "So",
    "Zs", "Zl", "Zp",
    "Cc", "Cf", "Cs", "Co", "Cn",
]
GC_MAP = {v: i for i, v in enumerate(GC_VALUES)}

BIDI_CLASS_VALUES = [
    "L", "R", "AL",
    "EN", "ES", "ET", "AN", "CS",
    "NSM", "BN",
    "B", "S", "WS", "ON",
    "LRE", "LRO", "RLE", "RLO", "PDF",
    "LRI", "RLI", "FSI", "PDI",
]
BIDI_CLASS_MAP = {v: i for i, v in enumerate(BIDI_CLASS_VALUES)}

EAW_VALUES = ["N", "Na", "H", "W", "F", "A"]
EAW_MAP = {v: i for i, v in enumerate(EAW_VALUES)}

SCRIPT_VALUES = []  # Populated from Scripts.txt
SCRIPT_MAP = {}

BLOCK_VALUES = []  # Populated from Blocks.txt
BLOCK_MAP = {}

LINE_BREAK_VALUES = [
    "XX", "BK", "CR", "LF", "CM", "NL", "SG", "WJ",
    "ZW", "GL", "SP", "ZWJ",
    "B2", "BA", "BB", "HY", "CB",
    "CL", "CP", "EX", "IN", "NS", "OP", "QU",
    "IS", "NU", "PO", "PR", "SY",
    "AI", "AL", "CJ", "EB", "EM", "H2", "H3",
    "HL", "ID", "JL", "JT", "JV", "RI", "SA",
    "AK", "AP", "AS", "VF", "VI",
    "HH",
]
LINE_BREAK_MAP = {v: i for i, v in enumerate(LINE_BREAK_VALUES)}

GRAPHEME_BREAK_VALUES = [
    "Other", "CR", "LF", "Control", "Extend", "ZWJ",
    "Regional_Indicator", "Prepend", "SpacingMark",
    "L", "V", "T", "LV", "LVT",
]
GRAPHEME_BREAK_MAP = {v: i for i, v in enumerate(GRAPHEME_BREAK_VALUES)}

INCB_VALUES = ["None", "Consonant", "Extend", "Linker"]
INCB_MAP = {v: i for i, v in enumerate(INCB_VALUES)}

WORD_BREAK_VALUES = [
    "Other", "CR", "LF", "Newline", "Extend", "ZWJ",
    "Regional_Indicator", "Format", "Katakana",
    "Hebrew_Letter", "ALetter", "Single_Quote",
    "Double_Quote", "MidNumLet", "MidLetter",
    "MidNum", "Numeric", "ExtendNumLet", "WSegSpace",
]
WORD_BREAK_MAP = {v: i for i, v in enumerate(WORD_BREAK_VALUES)}

SENTENCE_BREAK_VALUES = [
    "Other", "CR", "LF", "Extend", "Sep", "Format",
    "Sp", "Lower", "Upper", "OLetter", "Numeric",
    "ATerm", "STerm", "Close", "SContinue",
]
SENTENCE_BREAK_MAP = {v: i for i, v in enumerate(SENTENCE_BREAK_VALUES)}

JOINING_TYPE_VALUES = ["U", "C", "D", "L", "R", "T"]
JOINING_TYPE_MAP = {v: i for i, v in enumerate(JOINING_TYPE_VALUES)}

# Binary properties — packed into uint64_t bitmask per codepoint
BINARY_PROPERTIES = [
    "White_Space", "Alphabetic", "Noncharacter_Code_Point",
    "Default_Ignorable_Code_Point", "Deprecated", "Logical_Order_Exception",
    "Variation_Selector", "Uppercase", "Lowercase",
    "Soft_Dotted", "Case_Ignorable", "Cased",
    "Changes_When_Lowercased", "Changes_When_Uppercased",
    "Changes_When_Titlecased", "Changes_When_Casefolded",
    "Changes_When_Casemapped", "ID_Start", "ID_Continue",
    "XID_Start", "XID_Continue", "Pattern_Syntax",
    "Pattern_White_Space", "Dash", "Quotation_Mark",
    "Terminal_Punctuation", "Sentence_Terminal",
    "Diacritic", "Extender", "Grapheme_Base",
    "Grapheme_Extend", "Grapheme_Link", "Math",
    "Hex_Digit", "ASCII_Hex_Digit", "Ideographic",
    "Unified_Ideograph", "Radical", "IDS_Binary_Operator",
    "IDS_Trinary_Operator", "Join_Control",
    "Emoji", "Emoji_Presentation", "Emoji_Modifier",
    "Emoji_Modifier_Base", "Emoji_Component",
    "Extended_Pictographic",
]
BINARY_PROP_MAP = {v: i for i, v in enumerate(BINARY_PROPERTIES)}

# Quick_Check values: Yes=0, Maybe=1, No=2
# Pack NFC_QC (2 bits) | NFD_QC (2 bits) | NFKC_QC (2 bits) | NFKD_QC (2 bits)
# into one uint8_t

# ---------------------------------------------------------------------------
# Two-stage table builder
# ---------------------------------------------------------------------------

class TwoStageTable:
    """Build a two-stage lookup table for per-codepoint data."""

    def __init__(self, name, c_type, default_value=0):
        self.name = name
        self.c_type = c_type
        self.default_value = default_value
        self.data = [default_value] * MAX_CP

    def set(self, cp, value):
        if 0 <= cp < MAX_CP:
            self.data[cp] = value

    def set_range(self, start, end, value):
        for cp in range(start, min(end + 1, MAX_CP)):
            self.data[cp] = value

    def build(self):
        """Return (stage1, stage2_flat, unique_blocks_count)."""
        blocks = []
        for i in range(NUM_BLOCKS):
            block = tuple(self.data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE])
            blocks.append(block)

        # Deduplicate blocks
        unique = {}
        stage1 = []
        for block in blocks:
            if block not in unique:
                unique[block] = len(unique)
            stage1.append(unique[block])

        # Flatten stage2
        index_to_block = {v: k for k, v in unique.items()}
        stage2 = []
        for i in range(len(unique)):
            stage2.extend(index_to_block[i])

        return stage1, stage2, len(unique)

    def generate_n00b(self, out_path):
        """Write the n00b source file."""
        stage1, stage2, n_unique = self.build()
        print(f"  {self.name}: {n_unique} unique blocks, "
              f"stage1={NUM_BLOCKS * 2}B, "
              f"stage2={len(stage2) * self._type_size()}B")

        with open(out_path, "w") as f:
            f.write(TABLE_PROLOGUE)
            if self.c_type == "uint8_t":
                f.write(DATA_FILE_IMPORT)

            s1_type = "u16" if n_unique < 65536 else "u32"
            f.write(f"public const {self.name}_stage1: Array[{s1_type}] = [\n")
            for i in range(0, NUM_BLOCKS, 16):
                vals = stage1[i:i + 16]
                f.write("    " + ", ".join(str(v) for v in vals) + ",\n")
            f.write("]\n\n")

            if self.c_type == "uint8_t":
                bin_path = out_path.with_name(out_path.stem + "_stage2.bin")
                bin_path.write_bytes(bytes(stage2))
                f.write(f"public const {self.name}_stage2: Array[u8] = "
                        f'sys.fs.data_file(source_dir() + "/{bin_path.name}")\n')
            else:
                f.write(f"public const {self.name}_stage2: "
                        f"Array[{N00B_TYPES[self.c_type]}] = [\n")
                for i in range(0, len(stage2), 16):
                    vals = stage2[i:i + 16]
                    if self.c_type == "uint64_t":
                        f.write("    " + ", ".join(f"0x{v:016x}" for v in vals) + ",\n")
                    else:
                        f.write("    " + ", ".join(str(v) for v in vals) + ",\n")
                f.write("]\n")

        return n_unique

    def _type_size(self):
        sizes = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8, "int8_t": 1}
        return sizes.get(self.c_type, 1)


# ---------------------------------------------------------------------------
# Sparse mapping builder
# ---------------------------------------------------------------------------

class SparseMapping:
    """Build a sorted array for sparse codepoint→data mappings."""

    def __init__(self, name):
        self.name = name
        self.entries = {}  # cp → data (list of uint32_t or codepoints)

    def add(self, cp, data):
        self.entries[cp] = data

    def generate_n00b(self, out_path, append=False):
        """Write sorted mapping + data array."""
        sorted_cps = sorted(self.entries.keys())
        # Build data array
        data_array = []
        offsets = {}
        for cp in sorted_cps:
            offsets[cp] = len(data_array)
            vals = self.entries[cp]
            # Store length first, then values
            data_array.append(len(vals))
            data_array.extend(vals)

        mode = "a" if append else "w"
        with open(out_path, mode) as f:
            if not append:
                f.write(TABLE_PROLOGUE)

            f.write(f"public const {self.name}_index: Array[Array[u32]] = [\n")
            for cp in sorted_cps:
                f.write(f"    [0x{cp:06X}, {offsets[cp]}],\n")
            f.write("]\n")
            f.write(f"public const {self.name}_index_len: u32 = {len(sorted_cps)}\n\n")

            f.write(f"public const {self.name}_data: Array[u32] = [\n")
            for i in range(0, len(data_array), 16):
                vals = data_array[i:i + 16]
                f.write("    " + ", ".join(
                    f"0x{v & 0xFFFFFFFF:08X}" for v in vals) + ",\n")
            f.write("]\n\n")

        print(f"  {self.name}: {len(sorted_cps)} entries, "
              f"{len(data_array)} data words")


# ---------------------------------------------------------------------------
# Generators for each property
# ---------------------------------------------------------------------------

def gen_categories(cache_dir, out_dir):
    """Generate General_Category two-stage table from UnicodeData.txt."""
    print("Generating categories.n ...")
    table = TwoStageTable("gc", "uint8_t", GC_MAP["Cn"])

    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    for cp, parts in ud.items():
        gc = parts[2] if len(parts) > 2 else "Cn"
        if gc in GC_MAP:
            table.set(cp, GC_MAP[gc])

    table.generate_n00b(out_dir / "categories.n")


def gen_combining(cache_dir, out_dir):
    """Generate Canonical_Combining_Class table from UnicodeData.txt."""
    print("Generating combining.n ...")
    table = TwoStageTable("ccc", "uint8_t", 0)

    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    for cp, parts in ud.items():
        ccc = int(parts[3]) if len(parts) > 3 and parts[3] else 0
        table.set(cp, ccc)

    table.generate_n00b(out_dir / "combining.n")


def gen_bidi_class(cache_dir, out_dir):
    """Generate Bidi_Class table from DerivedBidiClass.txt."""
    print("Generating bidi_class.n ...")
    table = TwoStageTable("bidi", "uint8_t", BIDI_CLASS_MAP["L"])

    aliases = property_value_aliases(cache_dir, "bc")
    path = cache_dir / "extracted_DerivedBidiClass.txt"

    for (start, end), fields in (parse_missing_defaults(path)
                                 + list(parse_semicolon_file(path))):
        bc = aliases.get(fields[0]) if fields else None
        if bc in BIDI_CLASS_MAP:
            table.set_range(start, end, BIDI_CLASS_MAP[bc])

    table.generate_n00b(out_dir / "bidi_class.n")


def gen_bidi_brackets(cache_dir, out_dir):
    """Generate BidiBrackets tables."""
    print("Generating bidi_brackets.n ...")

    brackets = SparseMapping("bidi_bracket")
    for (start, end), fields in parse_semicolon_file(cache_dir / "BidiBrackets.txt"):
        if len(fields) >= 2:
            paired = int(fields[0], 16)
            bracket_type = 1 if fields[1] == "o" else 2  # 1=open, 2=close
            brackets.add(start, [paired, bracket_type])

    brackets.generate_n00b(out_dir / "bidi_brackets.n")


def gen_scripts(cache_dir, out_dir):
    """Generate Script two-stage table from Scripts.txt."""
    print("Generating scripts.n ...")

    # First pass: collect all script names
    path = cache_dir / "Scripts.txt"
    entries = parse_missing_defaults(path) + list(parse_semicolon_file(path))
    scripts = set()
    for (start, end), fields in entries:
        if fields:
            scripts.add(fields[0])

    global SCRIPT_VALUES, SCRIPT_MAP
    SCRIPT_VALUES = sorted(scripts)
    # Put "Unknown" (or "Common") first
    if "Common" in SCRIPT_VALUES:
        SCRIPT_VALUES.remove("Common")
        SCRIPT_VALUES.insert(0, "Common")
    if "Unknown" in SCRIPT_VALUES:
        SCRIPT_VALUES.remove("Unknown")
        SCRIPT_VALUES.insert(0, "Unknown")
    SCRIPT_MAP = {v: i for i, v in enumerate(SCRIPT_VALUES)}

    table = TwoStageTable("script", "uint8_t", SCRIPT_MAP.get("Unknown", 0))
    for (start, end), fields in entries:
        if fields and fields[0] in SCRIPT_MAP:
            table.set_range(start, end, SCRIPT_MAP[fields[0]])

    n = table.generate_n00b(out_dir / "scripts.n")

    short_to_long = {}

    for line in open(cache_dir / "PropertyValueAliases.txt",
                     encoding="utf-8"):
        line = line.split("#", 1)[0].strip()

        if not line.startswith("sc ;"):
            continue

        parts = [x.strip() for x in line.split(";")]

        if len(parts) >= 3:
            short_to_long[parts[1]] = parts[2]

    recommended = [0] * len(SCRIPT_VALUES)
    unmapped = []
    meta = cache_dir / "scriptMetadata.txt"

    if is_nonempty_file(meta):
        for line in open(meta, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()

            if not line:
                continue

            cols = [x.strip() for x in line.split(";")]

            if len(cols) < 6 or cols[5] != "RECOMMENDED":
                continue

            long_name = short_to_long.get(cols[0])

            if long_name is None or long_name not in SCRIPT_MAP:
                unmapped.append(cols[0])
                continue

            recommended[SCRIPT_MAP[long_name]] = 1
    else:
        fail("scriptMetadata.txt is required for the UTS #39 recommended "
             "script set")

    print(f"  scripts: {sum(recommended)} recommended (UTS #39 via CLDR), "
          f"{len(unmapped)} CLDR-only codes with no UCD script value: "
          f"{', '.join(sorted(unmapped))}")

    rec_bin = out_dir / "scripts_recommended.bin"
    rec_bin.write_bytes(bytes(recommended))

    with open(out_dir / "scripts.n", "a") as f:
        f.write(f"\npublic const script_recommended: Array[u8] = "
                f'sys.fs.data_file(source_dir() + "/{rec_bin.name}")\n')

    with open(out_dir / "scripts.n", "a") as f:
        f.write(f"\npublic const script_names: Array[string] = [\n")
        for s in SCRIPT_VALUES:
            f.write(f'    "{s}",\n')
        f.write("]\n")
        f.write(f"public const script_count: u32 = {len(SCRIPT_VALUES)}\n")


def gen_script_extensions(cache_dir, out_dir):
    """Generate Script_Extensions sparse table from ScriptExtensions.txt."""
    print("Generating script_extensions.n ...")

    # Build abbreviation → full-name map from PropertyValueAliases.txt
    abbrev_map = {}
    pva_path = cache_dir / "PropertyValueAliases.txt"
    require_nonempty_file(pva_path, "Unicode input file")
    with open(pva_path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) >= 3 and parts[0] == "sc":
                abbr = parts[1]  # 4-letter abbreviation
                full = parts[2]  # Full name
                abbrev_map[abbr] = full

    # Parse ScriptExtensions.txt
    scext = SparseMapping("script_ext")
    path = cache_dir / "ScriptExtensions.txt"
    require_nonempty_file(path, "Unicode input file")
    with open(path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 2:
                continue
            cp_field = parts[0]
            abbrs = parts[1].split()

            # Map abbreviations to script indices
            indices = []
            for abbr in abbrs:
                full = abbrev_map.get(abbr, abbr)
                if full in SCRIPT_MAP:
                    indices.append(SCRIPT_MAP[full])

            if not indices:
                continue

            indices.sort()

            if ".." in cp_field:
                start, end = cp_field.split("..")
                for cp in range(int(start, 16), int(end, 16) + 1):
                    scext.add(cp, indices)
            else:
                scext.add(int(cp_field, 16), indices)

    # Find Common and Inherited indices
    common_idx = SCRIPT_MAP.get("Common", 0)
    inherited_idx = SCRIPT_MAP.get("Inherited", 0)

    with open(out_dir / "script_extensions.n", "w") as f:
        f.write(TABLE_PROLOGUE)
        f.write(f"public const script_common = {common_idx}\n")
        f.write(f"public const script_inherited = {inherited_idx}\n\n")

    scext.generate_n00b(out_dir / "script_extensions.n", append=True)

    print(f"  script_extensions: Common={common_idx}, Inherited={inherited_idx}")


def gen_blocks(cache_dir, out_dir):
    """Generate Block ranges from Blocks.txt."""
    print("Generating blocks.n ...")

    global BLOCK_VALUES, BLOCK_MAP
    blocks = []
    for (start, end), fields in parse_semicolon_file(cache_dir / "Blocks.txt"):
        if fields:
            name = fields[0]
            blocks.append((start, end, name))

    BLOCK_VALUES = ["No_Block"] + [b[2] for b in blocks]
    BLOCK_MAP = {v: i for i, v in enumerate(BLOCK_VALUES)}

    with open(out_dir / "blocks.n", "w") as f:
        f.write(TABLE_PROLOGUE)

        f.write(f"public const block_ranges: Array[Array[u32]] = [\n")
        for start, end, name in blocks:
            f.write(f"    [0x{start:06X}, 0x{end:06X}],\n")
        f.write("]\n")
        f.write(f"public const block_ids: Array[u16] = [\n")
        for start, end, name in blocks:
            f.write(f"    {BLOCK_MAP[name]},\n")
        f.write("]\n")
        f.write(f"public const block_count: u32 = {len(blocks)}\n\n")

        f.write(f"public const block_names: Array[string] = [\n")
        for b in BLOCK_VALUES:
            f.write(f'    "{b}",\n')
        f.write("]\n")

    print(f"  blocks: {len(blocks)} block ranges")


def gen_eaw(cache_dir, out_dir):
    """Generate East_Asian_Width table."""
    print("Generating eaw.n ...")
    table = TwoStageTable("eaw", "uint8_t", EAW_MAP["N"])

    path = cache_dir / "EastAsianWidth.txt"

    for (start, end), fields in (parse_missing_defaults(path)
                                 + list(parse_semicolon_file(path))):
        if fields and fields[0] in EAW_MAP:
            table.set_range(start, end, EAW_MAP[fields[0]])

    table.generate_n00b(out_dir / "eaw.n")


def gen_linebreak(cache_dir, out_dir):
    """Generate Line_Break property table."""
    print("Generating linebreak.n ...")
    table = TwoStageTable("lb", "uint8_t", LINE_BREAK_MAP["XX"])

    unknown = set()
    path = cache_dir / "LineBreak.txt"

    for (start, end), fields in (parse_missing_defaults(path)
                                 + list(parse_semicolon_file(path))):
        if not fields:
            continue

        if fields[0] in LINE_BREAK_MAP:
            table.set_range(start, end, LINE_BREAK_MAP[fields[0]])
        else:
            unknown.add(fields[0])

    if unknown:
        fail(f"LineBreak.txt has classes this generator does not know: "
             f"{', '.join(sorted(unknown))}")

    table.generate_n00b(out_dir / "linebreak.n")


def gen_grapheme(cache_dir, out_dir):
    """Generate Grapheme_Cluster_Break table."""
    print("Generating grapheme.n ...")
    table = TwoStageTable("gcb", "uint8_t", GRAPHEME_BREAK_MAP["Other"])

    for (start, end), fields in parse_semicolon_file(
            cache_dir / "auxiliary_GraphemeBreakProperty.txt"):
        if fields and fields[0] in GRAPHEME_BREAK_MAP:
            table.set_range(start, end, GRAPHEME_BREAK_MAP[fields[0]])

    table.generate_n00b(out_dir / "grapheme.n")

    incb = TwoStageTable("incb", "uint8_t", INCB_MAP["None"])

    for (start, end), fields in parse_semicolon_file(
            cache_dir / "DerivedCoreProperties.txt"):
        if len(fields) >= 2 and fields[0] == "InCB" and fields[1] in INCB_MAP:
            incb.set_range(start, end, INCB_MAP[fields[1]])

    incb.generate_n00b(out_dir / "incb.n")


def gen_wordbreak(cache_dir, out_dir):
    """Generate Word_Break table."""
    print("Generating wordbreak.n ...")
    table = TwoStageTable("wb", "uint8_t", WORD_BREAK_MAP["Other"])

    for (start, end), fields in parse_semicolon_file(
            cache_dir / "auxiliary_WordBreakProperty.txt"):
        if fields and fields[0] in WORD_BREAK_MAP:
            table.set_range(start, end, WORD_BREAK_MAP[fields[0]])

    table.generate_n00b(out_dir / "wordbreak.n")


def gen_sentbreak(cache_dir, out_dir):
    """Generate Sentence_Break table."""
    print("Generating sentbreak.n ...")
    table = TwoStageTable("sb", "uint8_t", SENTENCE_BREAK_MAP["Other"])

    for (start, end), fields in parse_semicolon_file(
            cache_dir / "auxiliary_SentenceBreakProperty.txt"):
        if fields and fields[0] in SENTENCE_BREAK_MAP:
            table.set_range(start, end, SENTENCE_BREAK_MAP[fields[0]])

    table.generate_n00b(out_dir / "sentbreak.n")


def gen_joining(cache_dir, out_dir):
    """Generate Joining_Type table from ArabicShaping.txt."""
    print("Generating joining.n ...")
    table = TwoStageTable("jt", "uint8_t", JOINING_TYPE_MAP["U"])

    # UAX #44 derives the Joining_Type default rather than listing it:
    # Mn, Me and Cf default to Transparent, everything else to Non_Joining.
    # ArabicShaping.txt lists only the exceptions, so seed T first and let
    # the explicit entries below overwrite it.
    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    derived_t = 0

    for cp, parts in ud.items():
        gc = parts[2] if len(parts) > 2 else "Cn"

        if gc in ("Mn", "Me", "Cf"):
            table.set(cp, JOINING_TYPE_MAP["T"])
            derived_t += 1

    print(f"  joining: {derived_t} codepoints default to T from Mn/Me/Cf")

    # ArabicShaping.txt has format: cp ; name ; joining_type ; joining_group
    path = cache_dir / "ArabicShaping.txt"
    require_nonempty_file(path, "Unicode input file")
    with open(path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) >= 3:
                cp_str = parts[0]
                jt = parts[2]
                if ".." in cp_str:
                    start, end = cp_str.split("..")
                    for cp in range(int(start, 16), int(end, 16) + 1):
                        if jt in JOINING_TYPE_MAP:
                            table.set(cp, JOINING_TYPE_MAP[jt])
                else:
                    cp = int(cp_str, 16)
                    if jt in JOINING_TYPE_MAP:
                        table.set(cp, JOINING_TYPE_MAP[jt])

    table.generate_n00b(out_dir / "joining.n")


def gen_age(cache_dir, out_dir):
    """Generate Age ranges from DerivedAge.txt."""
    print("Generating age.n ...")

    ages = set()
    entries = []
    for (start, end), fields in parse_semicolon_file(cache_dir / "DerivedAge.txt"):
        if fields:
            age = fields[0]
            ages.add(age)
            entries.append((start, end, age))

    age_list = sorted(ages, key=lambda a: tuple(int(x) for x in a.split(".")))
    age_map = {v: i + 1 for i, v in enumerate(age_list)}  # 0 = unassigned
    age_list = ["Unassigned"] + age_list

    table = TwoStageTable("age", "uint8_t", 0)
    for start, end, age in entries:
        table.set_range(start, end, age_map[age])

    table.generate_n00b(out_dir / "age.n")

    with open(out_dir / "age.n", "a") as f:
        f.write(f"\npublic const age_names: Array[string] = [\n")
        for a in age_list:
            f.write(f'    "{a}",\n')
        f.write("]\n")
        f.write(f"public const age_count: u32 = {len(age_list)}\n")


def gen_proplist(cache_dir, out_dir):
    """Generate binary properties packed into uint64_t."""
    print("Generating proplist.n ...")
    table = TwoStageTable("props", "uint64_t", 0)

    # Parse PropList.txt
    for (start, end), fields in parse_semicolon_file(cache_dir / "PropList.txt"):
        if fields and fields[0] in BINARY_PROP_MAP:
            bit = BINARY_PROP_MAP[fields[0]]
            for cp in range(start, end + 1):
                if cp < MAX_CP:
                    table.data[cp] |= (1 << bit)

    # Parse DerivedCoreProperties.txt
    for (start, end), fields in parse_semicolon_file(
            cache_dir / "DerivedCoreProperties.txt"):
        if fields and fields[0] in BINARY_PROP_MAP:
            bit = BINARY_PROP_MAP[fields[0]]
            for cp in range(start, end + 1):
                if cp < MAX_CP:
                    table.data[cp] |= (1 << bit)

    # Parse emoji properties
    for (start, end), fields in parse_semicolon_file(
            cache_dir / "emoji_emoji-data.txt"):
        if fields and fields[0] in BINARY_PROP_MAP:
            bit = BINARY_PROP_MAP[fields[0]]
            for cp in range(start, end + 1):
                if cp < MAX_CP:
                    table.data[cp] |= (1 << bit)

    table.generate_n00b(out_dir / "proplist.n")


def gen_decomposition(cache_dir, out_dir):
    """Generate canonical + compatibility decomposition mappings."""
    print("Generating decomposition.n ...")

    canonical = SparseMapping("canon_decomp")
    compat = SparseMapping("compat_decomp")

    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    for cp, parts in ud.items():
        if len(parts) <= 5:
            continue
        decomp = parts[5].strip()
        if not decomp:
            continue

        # Check for compatibility tag
        is_compat = False
        if decomp.startswith("<"):
            is_compat = True
            # Strip tag
            idx = decomp.index(">")
            decomp = decomp[idx + 1:].strip()

        cps = [int(x, 16) for x in decomp.split()]
        if not cps:
            continue

        if is_compat:
            compat.add(cp, cps)
        else:
            canonical.add(cp, cps)
            # Also add to compat (canonical decomps are also compat decomps)
            compat.add(cp, cps)

    canonical.generate_n00b(out_dir / "decomposition.n")
    compat.generate_n00b(out_dir / "decomposition.n", append=True)

    widest = max((len(v) for v in compat.entries.values()), default=0)
    widest = max(widest, max((len(v) for v in canonical.entries.values()),
                             default=0))

    with open(out_dir / "decomposition.n", "a") as f:
        f.write(f"\npublic const max_decomp = {widest}\n")


def gen_composition(cache_dir, out_dir):
    """Generate canonical composition hash table."""
    print("Generating composition.n ...")

    exclusions = set()
    for (start, end), fields in parse_semicolon_file(
            cache_dir / "DerivedNormalizationProps.txt"):
        if fields and fields[0] == "Full_Composition_Exclusion":
            for cp in range(start, end + 1):
                exclusions.add(cp)

    # Build composition pairs from UnicodeData.txt canonical decompositions
    compositions = {}  # (starter, combining) → composed
    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    for cp, parts in ud.items():
        if len(parts) <= 5:
            continue
        decomp = parts[5].strip()
        if not decomp or decomp.startswith("<"):
            continue

        cps = [int(x, 16) for x in decomp.split()]
        if len(cps) != 2:
            continue  # Only binary decompositions compose

        if cp in exclusions:
            continue

        starter, combining = cps[0], cps[1]
        compositions[(starter, combining)] = cp

    # Build open-addressing hash table
    n_entries = len(compositions)
    table_size = 1
    while table_size < n_entries * 2:
        table_size *= 2

    EMPTY = 0xFFFFFFFF
    keys = [(EMPTY, EMPTY)] * table_size
    values = [0] * table_size

    for (starter, combining), composed in compositions.items():
        key = (starter << 21) | combining
        h = key % table_size
        while keys[h] != (EMPTY, EMPTY):
            h = (h + 1) % table_size
        keys[h] = (starter, combining)
        values[h] = composed

    with open(out_dir / "composition.n", "w") as f:
        f.write(TABLE_PROLOGUE)

        f.write(f"public const comp_table_size = {table_size}\n")
        f.write(f"public const comp_empty = 0xFFFFFFFF\n\n")

        f.write(f"public const comp_starter: Array[u32] = [\n")
        for i in range(0, table_size, 8):
            vals = [f"0x{keys[j][0]:08X}" for j in range(i, min(i + 8, table_size))]
            f.write("    " + ", ".join(vals) + ",\n")
        f.write("]\n\n")

        f.write(f"public const comp_combining: Array[u32] = [\n")
        for i in range(0, table_size, 8):
            vals = [f"0x{keys[j][1]:08X}" for j in range(i, min(i + 8, table_size))]
            f.write("    " + ", ".join(vals) + ",\n")
        f.write("]\n\n")

        f.write(f"public const comp_result: Array[u32] = [\n")
        for i in range(0, table_size, 8):
            vals = [f"0x{values[j]:06X}" for j in range(i, min(i + 8, table_size))]
            f.write("    " + ", ".join(vals) + ",\n")
        f.write("]\n")

    print(f"  composition: {n_entries} pairs in {table_size}-slot table "
          f"({n_entries * 100 / table_size:.0f}% load)")


def gen_normprops(cache_dir, out_dir):
    """Generate NFC/NFD/NFKC/NFKD Quick_Check values."""
    print("Generating normprops.n ...")

    # QC values: Y=0, M=1, N=2
    # Pack: bits 0-1=NFC_QC, 2-3=NFD_QC, 4-5=NFKC_QC, 6-7=NFKD_QC
    table = TwoStageTable("nqc", "uint8_t", 0)

    qc_map = {"Y": 0, "M": 1, "N": 2}
    form_shift = {"NFC_QC": 0, "NFD_QC": 2, "NFKC_QC": 4, "NFKD_QC": 6}

    for (start, end), fields in parse_semicolon_file(
            cache_dir / "DerivedNormalizationProps.txt"):
        if not fields:
            continue
        prop = fields[0]
        if prop in form_shift:
            val = qc_map.get(fields[1] if len(fields) > 1 else "Y", 0)
            shift = form_shift[prop]
            for cp in range(start, end + 1):
                if cp < MAX_CP:
                    table.data[cp] = (table.data[cp] & ~(3 << shift)) | (val << shift)

    table.generate_n00b(out_dir / "normprops.n")


def gen_casemap(cache_dir, out_dir):
    """Generate simple + full case mappings + case folding."""
    print("Generating casemap.n ...")

    # Simple case mappings from UnicodeData.txt fields 12, 13, 14
    simple_upper = SparseMapping("simple_upper")
    simple_lower = SparseMapping("simple_lower")
    simple_title = SparseMapping("simple_title")

    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")
    for cp, parts in ud.items():
        if len(parts) > 12 and parts[12].strip():
            simple_upper.add(cp, [int(parts[12].strip(), 16)])
        if len(parts) > 13 and parts[13].strip():
            simple_lower.add(cp, [int(parts[13].strip(), 16)])
        if len(parts) > 14 and parts[14].strip():
            simple_title.add(cp, [int(parts[14].strip(), 16)])

    # Full case mappings from SpecialCasing.txt
    full_upper = SparseMapping("full_upper")
    full_lower = SparseMapping("full_lower")
    full_title = SparseMapping("full_title")
    final_sigma_lower = SparseMapping("final_sigma_lower")

    path = cache_dir / "SpecialCasing.txt"
    require_nonempty_file(path, "Unicode input file")
    with open(path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 4:
                continue

            cp = int(parts[0], 16)
            lower_cps = [int(x, 16) for x in parts[1].split()] if parts[1].strip() else []

            if len(parts) >= 5 and parts[4].split() == ["Final_Sigma"]:
                final_sigma_lower.add(cp, lower_cps)
                continue
            # Skip conditional mappings (have a 5th field with conditions)
            if len(parts) >= 5 and parts[4].strip():
                continue
            title_cps = [int(x, 16) for x in parts[2].split()] if parts[2].strip() else []
            upper_cps = [int(x, 16) for x in parts[3].split()] if parts[3].strip() else []

            if lower_cps:
                full_lower.add(cp, lower_cps)
            if title_cps:
                full_title.add(cp, title_cps)
            if upper_cps:
                full_upper.add(cp, upper_cps)

    # Case folding from CaseFolding.txt
    simple_fold = SparseMapping("casefold_simple")
    full_fold = SparseMapping("casefold_full")

    for (start, end), fields in parse_semicolon_file(cache_dir / "CaseFolding.txt"):
        if len(fields) < 2:
            continue
        status = fields[0]
        mapping = [int(x, 16) for x in fields[1].split()]

        if status == "C" or status == "S":
            # Common/Simple — single codepoint
            simple_fold.add(start, mapping)
        if status == "C" or status == "F":
            # Common/Full — possibly multi-codepoint
            full_fold.add(start, mapping)

    with open(out_dir / "casemap.n", "w") as f:
        f.write(TABLE_PROLOGUE)

    simple_upper.generate_n00b(out_dir / "casemap.n", append=True)
    simple_lower.generate_n00b(out_dir / "casemap.n", append=True)
    simple_title.generate_n00b(out_dir / "casemap.n", append=True)
    full_upper.generate_n00b(out_dir / "casemap.n", append=True)
    full_lower.generate_n00b(out_dir / "casemap.n", append=True)
    full_title.generate_n00b(out_dir / "casemap.n", append=True)
    simple_fold.generate_n00b(out_dir / "casemap.n", append=True)
    full_fold.generate_n00b(out_dir / "casemap.n", append=True)
    final_sigma_lower.generate_n00b(out_dir / "casemap.n", append=True)


def gen_confusables(cache_dir, out_dir):
    """Generate confusable mappings for skeleton algorithm."""
    print("Generating confusables.n ...")

    confusables = SparseMapping("confusable")

    path = cache_dir / "confusables.txt"
    require_nonempty_file(path, "Unicode input file")
    with open(path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 3:
                continue
            # Format: source ; target ; type
            src_cps = [int(x, 16) for x in parts[0].split()]
            tgt_cps = [int(x, 16) for x in parts[1].split()]

            if len(src_cps) == 1:
                confusables.add(src_cps[0], tgt_cps)

    confusables.generate_n00b(out_dir / "confusables.n")


def gen_identifiers(cache_dir, out_dir):
    """Generate Identifier_Status/Type tables."""
    print("Generating identifiers.n ...")

    # Identifier_Status: Allowed=1, Restricted=0
    table = TwoStageTable("id_status", "uint8_t", 0)
    path = cache_dir / "IdentifierStatus.txt"

    for (start, end), fields in (parse_missing_defaults(path)
                                 + list(parse_semicolon_file(path))):
        if fields:
            val = 1 if fields[0] == "Allowed" else 0
            table.set_range(start, end, val)

    table.generate_n00b(out_dir / "identifiers.n")


def gen_idna(cache_dir, out_dir):
    """Generate IDNA mapping table."""
    print("Generating idna.n ...")

    # IDNA statuses: valid=0, ignored=1, mapped=2, deviation=3, disallowed=4,
    #                disallowed_STD3_valid=5, disallowed_STD3_mapped=6
    idna_status_map = {
        "valid": 0, "ignored": 1, "mapped": 2, "deviation": 3,
        "disallowed": 4, "disallowed_STD3_valid": 5,
        "disallowed_STD3_mapped": 6,
    }

    status_table = TwoStageTable("idna_status", "uint8_t", 4)  # default: disallowed
    mapping = SparseMapping("idna_map")

    path = cache_dir / "IdnaMappingTable.txt"
    for (start, end), fields in parse_semicolon_file(path):
        if not fields:
            continue
        status = fields[0]
        if status in idna_status_map:
            status_table.set_range(start, end, idna_status_map[status])

            # If there's a mapping
            if len(fields) > 1 and fields[1].strip():
                map_cps = [int(x, 16) for x in fields[1].split()]
                for cp in range(start, end + 1):
                    mapping.add(cp, map_cps)

    status_table.generate_n00b(out_dir / "idna.n")
    mapping.generate_n00b(out_dir / "idna.n", append=True)


def gen_names(cache_dir, out_dir):
    """Generate Unicode codepoint→name and name→codepoint tables from
    UnicodeData.txt.

    Skips range placeholders (`<CJK Ideograph>`, `<Hangul Syllable>`,
    etc.) — those codepoints have algorithmically-computed names that
    a future revision can layer on top.  Explicit-name codepoints
    (ASCII, Latin, Cyrillic, Greek, symbols, emoji-style names, etc.)
    are emitted into a sorted array of (cp, name-offset) records plus
    a packed string pool of NUL-terminated names.
    """
    print(f"Generating {out_dir}/names.n")
    ud = parse_unicode_data(cache_dir / "UnicodeData.txt")

    # Filter to explicit names (skip <...> placeholders, control char
    # entries with empty names, etc.).
    entries = []
    for cp in sorted(ud.keys()):
        name = ud[cp][1]
        if not name or name.startswith("<"):
            continue
        entries.append((cp, name))

    # Build a string pool with offsets.  Names share no prefixes
    # in UnicodeData so a simple concat is fine.
    pool = bytearray()
    offsets = []
    for cp, name in entries:
        offsets.append(len(pool))
        pool.extend(name.encode("ascii"))
        pool.append(0)

    out_path = out_dir / "names.n"
    with open(out_path, "w") as f:
        f.write(TABLE_PROLOGUE)
        f.write(DATA_FILE_IMPORT)

        (out_dir / "names_pool.bin").write_bytes(bytes(pool))

        index = bytearray()

        for (cp, _name), off in zip(entries, offsets):
            index += cp.to_bytes(4, "little")
            index += off.to_bytes(4, "little")

        (out_dir / "names_index.bin").write_bytes(bytes(index))

        pool_path  = (out_dir / "names_pool.bin").resolve()
        index_path = (out_dir / "names_index.bin").resolve()

        f.write('public const name_pool: Array[u8] = '
                f'sys.fs.data_file(source_dir() + "/{pool_path.name}")\n\n')

        f.write('public const name_index: Array[u8] = '
                f'sys.fs.data_file(source_dir() + "/{index_path.name}")\n\n')

        by_name = bytearray()

        for cp, name, off in sorted(
                ((cp, name, off)
                 for (cp, name), off in zip(entries, offsets)),
                key=lambda t: t[1].encode("ascii")):
            by_name += off.to_bytes(4, "little")
            by_name += cp.to_bytes(4, "little")

        name_sorted_path = out_dir / "names_by_name.bin"
        name_sorted_path.write_bytes(bytes(by_name))

        f.write('public const name_by_name: Array[u8] = '
                f'sys.fs.data_file(source_dir() + "/{name_sorted_path.name}")\n\n')

        f.write(f'public const name_count = '
                f'{len(entries)}\n')


def _parse_break_suite(path):
    """Parse a UAX segmentation test file into (cps, boundaries) cases.

    Each line alternates a boundary marker with a codepoint, starting and
    ending with a marker.  U+00F7 marks a break, U+00D7 a non-break.
    """
    cases = []

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()

            if not line:
                continue

            cps = []
            bounds = []
            ok = True

            for tok in line.split():
                if tok == "÷":
                    bounds.append(1)
                elif tok == "×":
                    bounds.append(0)
                else:
                    try:
                        cps.append(int(tok, 16))
                    except ValueError:
                        ok = False
                        break

            if ok and cps and len(bounds) == len(cps) + 1:
                cases.append((cps, bounds))

    return cases


def _encode_break_suite(cases):
    blob = bytearray()
    blob += len(cases).to_bytes(4, "little")

    for cps, bounds in cases:
        blob += len(cps).to_bytes(4, "little")

        for cp in cps:
            blob += cp.to_bytes(4, "little")

        blob += bytes(bounds)

    return bytes(blob)


def _parse_norm_suite(path):
    cases = []

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()

            if not line or line.startswith("@"):
                continue

            fields = [x.strip() for x in line.split(";")]

            if len(fields) < 5:
                continue

            try:
                cols = [[int(c, 16) for c in fields[i].split()]
                        for i in range(5)]
            except ValueError:
                continue

            if all(cols):
                cases.append(cols)

    return cases


def _encode_norm_suite(cases):
    blob = bytearray()
    blob += len(cases).to_bytes(4, "little")

    for cols in cases:
        for col in cols:
            blob += len(col).to_bytes(4, "little")

            for cp in col:
                blob += cp.to_bytes(4, "little")

    return bytes(blob)


def _parse_bidi_char_suite(path):
    cases = []

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()

            if not line:
                continue

            parts = line.split(";")

            if len(parts) < 4:
                continue

            try:
                cps = [int(c, 16) for c in parts[0].split()]
                direction = int(parts[1])
                para = int(parts[2])
                levels = [255 if t == "x" else int(t)
                          for t in parts[3].split()]
            except ValueError:
                continue

            if cps and len(levels) == len(cps) and len(cps) <= 255:
                cases.append((cps, direction, para, levels))

    return cases


def _encode_bidi_char_suite(cases):
    blob = bytearray()
    blob += len(cases).to_bytes(4, "little")

    for cps, direction, para, levels in cases:
        blob += len(cps).to_bytes(4, "little")

        for cp in cps:
            blob += cp.to_bytes(4, "little")

        blob += bytes([direction, para])
        blob += bytes(levels)

    return bytes(blob)


def _parse_collation_suite(path):
    cases = []
    skipped = 0

    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist()
                 if n.endswith("CollationTest_NON_IGNORABLE.txt")]

        if not names:
            return cases

        text = z.read(names[0]).decode("utf-8", "replace")

    for raw in text.split("\n"):
        line = raw.split("#", 1)[0].strip()

        if not line:
            continue

        line = line.split(";")[0].strip()

        try:
            cps = [int(c, 16) for c in line.split()]
        except ValueError:
            continue

        if not cps or len(cps) > 32:
            continue

        if any(c >= 0x110000 or 0xD800 <= c <= 0xDFFF for c in cps):
            skipped += 1
            continue

        cases.append(cps)

    print(f"  collation suite: {skipped} lines skipped "
          f"(surrogate or out-of-range, not representable in utf-8)")

    return cases


def _encode_collation_suite(cases):
    blob = bytearray()
    blob += len(cases).to_bytes(4, "little")

    for cps in cases:
        blob += len(cps).to_bytes(4, "little")

        for cp in cps:
            blob += cp.to_bytes(4, "little")

    return bytes(blob)


def _idna_unescape(text):
    r"""IdnaTestV2 escapes characters as \uXXXX or \x{XXXX}.

    A column written as "" denotes the empty string, per the file header.
    """
    if text == '""':
        return ""

    out = []
    i = 0

    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] == "u":
            out.append(chr(int(text[i + 2:i + 6], 16)))
            i += 6
            continue

        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] == "x":
            close = text.index("}", i)
            out.append(chr(int(text[i + 3:close], 16)))
            i = close + 1
            continue

        out.append(text[i])
        i += 1

    return "".join(out)


IDNA_STATUS = re.compile(r"[A-Z][0-9A-Za-z_]*")

# Status codes this port does not act on, per the flag table in the
# IdnaTestV2.txt header. UseSTD3ASCIIRules is false, so U1 never fires.
# V4 only applies when CheckHyphens is false, and CheckHyphens is true here.
# X codes describe toUnicode results; this suite exercises toAsciiN.
IDNA_IGNORED_STATUS = {"U1", "V4"}


def _idna_expects_failure(status):
    """True when a row's status set contains an error this port implements."""
    codes = set(IDNA_STATUS.findall(status))
    codes -= IDNA_IGNORED_STATUS
    codes = {c for c in codes if not c.startswith("X")}

    return bool(codes)


def _parse_idna_suite(path):
    """IdnaTestV2 rows for toAsciiN, both the passes and the failures.

    Column 4 is toAsciiN and column 5 its status; a blank column 5 inherits
    column 3 (toUnicodeStatus), per the file header. A row whose surviving
    status set is non-empty expects toAscii to report an error.
    """
    cases = []
    skipped = 0

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0]

            if not line.strip():
                continue

            cols = [c.strip() for c in line.split(";")]

            if len(cols) < 5:
                continue

            source = _idna_unescape(cols[0])
            to_uni = _idna_unescape(cols[1]) if cols[1] else source
            to_ascii = _idna_unescape(cols[3]) if cols[3] else to_uni

            status = cols[4] if cols[4] else cols[2]
            expect_fail = _idna_expects_failure(status)

            if any(0xD800 <= ord(c) <= 0xDFFF for c in source):
                skipped += 1
                continue

            if not expect_fail and not to_ascii:
                skipped += 1
                continue

            src_cps = [ord(c) for c in source]
            dst_cps = [] if expect_fail else [ord(c) for c in to_ascii]

            if len(src_cps) > 255 or len(dst_cps) > 255:
                skipped += 1
                continue

            cases.append((src_cps, dst_cps, expect_fail))

    fails = sum(1 for c in cases if c[2])

    print(f"  idna suite: {len(cases)} cases "
          f"({fails} expect a validity failure), {skipped} skipped "
          f"(ill-formed or no expected output)")

    return cases


def _encode_idna_suite(cases):
    blob = bytearray()
    blob += len(cases).to_bytes(4, "little")

    for src, dst, expect_fail in cases:
        blob += (len(src) | (0x80000000 if expect_fail else 0)).to_bytes(
            4, "little")

        for cp in src:
            blob += cp.to_bytes(4, "little")

        blob += len(dst).to_bytes(4, "little")

        for cp in dst:
            blob += cp.to_bytes(4, "little")

    return bytes(blob)


def gen_versions(out_dir, versions):
    """Emit the UCD version the tables were built from."""
    print("Generating versions.n ...")

    with open(out_dir / "versions.n", "w") as f:
        f.write("public func version() -> string {\n")
        f.write(f'    return "{versions["ucd"]}"\n')
        f.write("}\n")


def gen_conformance(test_data_dir, conformance_dir, out_path, strict):
    """Embed the UCD conformance suites as binary blobs."""
    print(f"Generating {out_path.name} ...")

    missing = [name for name in REQUIRED_TEST_DATA_FILES
               if not is_nonempty_file(test_data_dir / name)]

    if missing:
        if strict:
            fail(f"Unicode test data missing: {', '.join(missing)}")
        print(f"  WARNING: Unicode test data missing "
              f"({', '.join(missing)}); {out_path.name} not emitted")
        return False

    data_dir = os.path.relpath(conformance_dir, out_path.parent)
    suites = [
        ("grapheme", "GraphemeBreakTest.txt"),
        ("word", "WordBreakTest.txt"),
        ("sentence", "SentenceBreakTest.txt"),
        ("line", "LineBreakTest.txt"),
    ]

    with open(out_path, "w") as f:
        f.write(TABLE_PROLOGUE)
        f.write(DATA_FILE_IMPORT)

        for name, fname in suites:
            cases = _parse_break_suite(test_data_dir / fname)
            blob = _encode_break_suite(cases)
            bin_path = conformance_dir / f"gen_conf_{name}.bin"
            bin_path.write_bytes(blob)

            print(f"  {name}: {len(cases)} cases, {len(blob)}B")

            f.write(f"public const conf_{name}: Array[u8] = sys.fs.data_file("
                    f'source_dir() + "/{data_dir}/{bin_path.name}")\n\n')

        cases = _parse_norm_suite(test_data_dir / "NormalizationTest.txt")
        blob = _encode_norm_suite(cases)
        bin_path = conformance_dir / "gen_conf_norm.bin"
        bin_path.write_bytes(blob)

        print(f"  normalization: {len(cases)} cases, {len(blob)}B")

        f.write("public const conf_norm: Array[u8] = sys.fs.data_file("
                f'source_dir() + "/{data_dir}/{bin_path.name}")\n\n')

        cases = _parse_bidi_char_suite(test_data_dir / "BidiCharacterTest.txt")
        blob = _encode_bidi_char_suite(cases)
        bin_path = conformance_dir / "gen_conf_bidi.bin"
        bin_path.write_bytes(blob)

        print(f"  bidi: {len(cases)} cases, {len(blob)}B")

        f.write("public const conf_bidi: Array[u8] = sys.fs.data_file("
                f'source_dir() + "/{data_dir}/{bin_path.name}")\n\n')

        cases = _parse_collation_suite(test_data_dir / "CollationTest.zip")
        blob = _encode_collation_suite(cases)
        bin_path = conformance_dir / "gen_conf_collate.bin"
        bin_path.write_bytes(blob)

        print(f"  collation: {len(cases)} cases, {len(blob)}B")

        f.write("public const conf_collate: Array[u8] = sys.fs.data_file("
                f'source_dir() + "/{data_dir}/{bin_path.name}")\n\n')

        cases = _parse_idna_suite(test_data_dir / "IdnaTestV2.txt")
        blob = _encode_idna_suite(cases)
        bin_path = conformance_dir / "gen_conf_idna.bin"
        bin_path.write_bytes(blob)

        print(f"  idna: {len(cases)} cases, {len(blob)}B")

        f.write("public const conf_idna: Array[u8] = sys.fs.data_file("
                f'source_dir() + "/{data_dir}/{bin_path.name}")\n')

    return True


def gen_collation(cache_dir, out_dir):
    """Generate DUCET collation data from allkeys.txt."""
    print("Generating collation.n ...")

    # Parse allkeys.txt format:
    # codepoint(s) ; [.primary.secondary.tertiary] [.p.s.t] ...
    # or: @implicitweights ranges

    single_ces = {}    # cp → list of (primary, secondary, tertiary)
    contractions = {}  # tuple of cps → list of (p, s, t)

    path = cache_dir / "allkeys.txt"
    if not is_nonempty_file(path):
        print("  WARNING: allkeys.txt not found, skipping collation")
        table = TwoStageTable("ducet", "uint32_t", 0xFFFFFFFF)
        table.generate_n00b(out_dir / "collation.n")
        with open(out_dir / "collation.n", "a") as f:
            f.write("\npublic const ce_data: Array[u16] = [0]\n")
            f.write("public const ce_data_len: u32 = 1\n")
            f.write("public const contraction_count: u32 = 0\n")
            f.write("public const contr_keys: Array[Array[u32]] = [[0, 0, 0, 0]]\n")
            f.write("public const contr_key_lens: Array[u32] = [0]\n")
            f.write("public const contr_ce_offsets: Array[u32] = [0]\n")
            f.write("public const contr_ce_data: Array[u16] = [0]\n")
            f.write("public const ducet_count: u32 = 0\n")
        return

    implicit_ranges = []

    with open(path, "r") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            if line.startswith("@implicitweights"):
                spec = line[len("@implicitweights"):].strip()
                rng, base = [x.strip() for x in spec.split(";")]
                lo, hi = [int(x, 16) for x in rng.split("..")]
                implicit_ranges.append((lo, hi, int(base, 16)))
                continue
            if line.startswith("@"):
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 2:
                continue

            cp_strs = parts[0].split()
            cps = [int(x, 16) for x in cp_strs]

            # Parse CE weights
            ce_str = parts[1]
            ces = []
            # Find all [.XXXX.XXXX.XXXX] or [*XXXX.XXXX.XXXX] patterns
            i = 0
            while i < len(ce_str):
                if ce_str[i] == '[':
                    end = ce_str.index(']', i)
                    inner = ce_str[i + 1:end]
                    # Replace * with . for splitting (variable weight marker)
                    inner = inner.replace("*", ".")
                    ce_parts = [x for x in inner.split(".") if x]
                    if len(ce_parts) >= 3:
                        ce_vals = [int(x, 16) for x in ce_parts[:3]]
                        ces.append((ce_vals[0], ce_vals[1], ce_vals[2]))
                    i = end + 1
                else:
                    i += 1

            if len(cps) == 1:
                single_ces[cps[0]] = ces
            else:
                contractions[tuple(cps)] = ces

    # Build two-stage table for single-codepoint → CE offset
    # CE data array stores: [count, p1, s1, t1, p2, s2, t2, ...]
    ce_data = []
    ce_offsets = {}

    # Deduplicate CE sequences
    ce_seq_map = {}
    for cp in sorted(single_ces.keys()):
        ces = single_ces[cp]
        key = tuple((p, s, t) for p, s, t in ces)
        if key not in ce_seq_map:
            ce_seq_map[key] = len(ce_data)
            ce_data.append(len(ces))
            for p, s, t in ces:
                ce_data.extend([p, s, t])
        ce_offsets[cp] = ce_seq_map[key]

    # Two-stage table mapping cp → offset into ce_data
    table = TwoStageTable("ducet", "uint32_t", 0xFFFFFFFF)
    for cp, offset in ce_offsets.items():
        table.set(cp, offset)

    table.generate_n00b(out_dir / "collation.n")

    with open(out_dir / "collation.n", "a") as f:
        f.write(f"\npublic const ce_data: Array[u16] = [\n")
        for i in range(0, len(ce_data), 16):
            vals = ce_data[i:i + 16]
            f.write("    " + ", ".join(f"0x{v:04X}" for v in vals) + ",\n")
        f.write("]\n")
        f.write(f"public const ce_data_len: u32 = {len(ce_data)}\n\n")

        sorted_contr = sorted(contractions.items())
        f.write(f"public const contraction_count: u32 = {len(sorted_contr)}\n\n")

        if sorted_contr:
            # Contraction index: {cp1, cp2, ..., 0, ce_offset}
            # For simplicity, store as flat array with sentinel
            contr_data = []
            contr_index = []
            for cps, ces in sorted_contr:
                offset = len(contr_data)
                contr_index.append((cps, offset))
                contr_data.append(len(ces))
                for p, s, t in ces:
                    contr_data.extend([p, s, t])

            f.write(f"public const contr_keys: Array[Array[u32]] = [\n")
            for cps, offset in contr_index:
                padded = list(cps) + [0] * (4 - len(cps))
                padded = padded[:4]
                f.write("    [{}],\n".format(
                    ", ".join(f"0x{v:06X}" for v in padded)))
            f.write("]\n\n")

            f.write(f"public const contr_key_lens: Array[u32] = [\n")
            for cps, offset in contr_index:
                f.write(f"    {len(cps)},\n")
            f.write("]\n\n")

            f.write(f"public const contr_ce_offsets: Array[u32] = [\n")
            for cps, offset in contr_index:
                f.write(f"    {offset},\n")
            f.write("]\n\n")

            f.write(f"public const contr_ce_data: Array[u16] = [\n")
            for i in range(0, len(contr_data), 16):
                vals = contr_data[i:i + 16]
                f.write("    " + ", ".join(f"0x{v:04X}" for v in vals) + ",\n")
            f.write("]\n\n")

            implicit_ranges.sort()

            f.write("public const implicit_ranges: Array[Array[u32]] = [\n")
            for lo, hi, base in implicit_ranges:
                f.write(f"    [0x{lo:06X}, 0x{hi:06X}, 0x{base:04X}],\n")
            if not implicit_ranges:
                f.write("    [0, 0, 0],\n")
            f.write("]\n\n")

            f.write("public const implicit_range_count: u32 = "
                    f"{len(implicit_ranges)}\n")

    core_han = []

    blocks_path = cache_dir / "Blocks.txt"

    if is_nonempty_file(blocks_path):
        for (start, end), fields in parse_semicolon_file(blocks_path):
            if not fields:
                continue
            if fields[0].strip() in ("CJK Unified Ideographs",
                                     "CJK Compatibility Ideographs"):
                core_han.append((start, end))

    core_han.sort()

    with open(out_dir / "collation.n", "a") as f:
        f.write("\npublic const core_han_ranges: Array[Array[u32]] = [\n")
        for lo, hi in core_han:
            f.write(f"    [0x{lo:06X}, 0x{hi:06X}],\n")
        if not core_han:
            f.write("    [0, 0],\n")
        f.write("]\n\n")
        f.write("public const core_han_range_count: u32 = "
                f"{len(core_han)}\n")

    print(f"  collation: {len(single_ces)} single entries, "
          f"{len(contractions)} contractions, "
          f"{len(ce_data)} CE data words, "
          f"{len(implicit_ranges)} implicit-weight ranges")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate UCD tables")
    parser.add_argument("--ucd-version", default="18.0.0",
                        help="UCD version (default: 18.0.0, synchronized with UCA/security/IDNA/emoji)")
    parser.add_argument("--uca-version", default="18.0.0",
                        help="UCA collation version (default: 18.0.0, synchronized with UCD)")
    parser.add_argument("--security-version", default="18.0.0",
                        help="UTS #39 security version (default: 18.0.0, synchronized with UCD)")
    parser.add_argument("--idna-version", default="18.0.0",
                        help="UTS #46 IDNA version (default: 18.0.0, synchronized with UCD)")
    parser.add_argument("--cldr-version", default="release-48",
                        help="CLDR tag/branch for scriptMetadata.txt")
    parser.add_argument("--emoji-version", default="18.0.0",
                        help="UTS #51 emoji version (default: 18.0.0, synchronized with UCD)")
    parser.add_argument("--cache-dir", default=".unicode_cache",
                        help="Cache directory for downloaded files")
    parser.add_argument("--out-dir", default="lib/unicode/generated",
                        help="Output directory for generated n00b table modules")
    parser.add_argument("--conformance-dir", default="tests/data/unicode",
                        help="Output directory for conformance suite blobs")
    parser.add_argument("--conformance-program",
                        default="tests/programs/unicode/gen_conformance.n",
                        help="Output path of the conformance n00b module")
    parser.add_argument("--test-data-dir", default="tests/data/unicode",
                        help="Output directory for test data files")
    parser.add_argument("--allow-downloads", action="store_true",
                        help="Allow network downloads for missing unicode cache/test files")
    parser.add_argument("--strict", dest="strict", action="store_true",
                        help="Fail when unicode test data files are missing; required cache files are always fatal")
    parser.add_argument("--no-strict", dest="strict", action="store_false",
                        help="Warn instead of failing when unicode test data files are missing")
    parser.add_argument("--check-inputs", action="store_true",
                        help="Populate/validate unicode cache and test data, then exit")
    parser.set_defaults(strict=True)
    args = parser.parse_args()

    versions = {
        "ucd":      args.ucd_version,
        "uca":      args.uca_version,
        "security": args.security_version,
        "idna":     args.idna_version,
        "emoji":    args.emoji_version,
        "cldr":     args.cldr_version,
    }

    # Resolve paths relative to CWD (works both standalone and from meson)
    cache_dir = UnicodeCache(Path(args.cache_dir).resolve(), versions)
    out_dir = Path(args.out_dir).resolve()
    conformance_dir = Path(args.conformance_dir).resolve()
    conformance_program = Path(args.conformance_program).resolve()
    test_data_dir = (Path(args.test_data_dir).resolve()
                     / f"ucd-{args.ucd_version}")

    for standard_dir in cache_dir.dirs.values():
        standard_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    conformance_dir.mkdir(parents=True, exist_ok=True)
    conformance_program.parent.mkdir(parents=True, exist_ok=True)
    test_data_dir.mkdir(parents=True, exist_ok=True)

    print(f"Unicode cache dir: {cache_dir.root}")
    print(f"Unicode test-data dir: {test_data_dir}")
    print(f"Allow downloads: {'yes' if args.allow_downloads else 'no'}")
    print(f"Strict input checks: {'yes' if args.strict else 'no'}")

    majors = {k: v.split(".")[0]
              for k, v in versions.items() if k != "cldr"}

    if len(set(majors.values())) != 1:
        detail = ", ".join(f"{k}={versions[k]}" for k in sorted(majors))
        fail("Unicode data pins disagree across major versions: " + detail
             + ". Since 18.0.0 these standards are published together and"
             + " must be pinned together.")

    missing_required, missing_test_data = download_all(versions,
                                                       cache_dir,
                                                       test_data_dir,
                                                       args.allow_downloads)
    print()

    if missing_required:
        print("ERROR: required unicode cache files are missing:")
        for name in missing_required:
            print(f"  - {name}")

        if args.allow_downloads:
            print("Downloads were enabled, but one or more required files could not be fetched.")
            print("Check network/DNS access and rerun.")
        else:
            print("Downloads are disabled. Pre-populate the cache directory above,")
            print("or rerun with --allow-downloads.")

        return 2

    validate_cache_inputs(cache_dir)

    if missing_test_data:
        label = "ERROR" if args.strict else "WARNING"
        print(f"{label}: unicode test-data files are missing:")
        for name in missing_test_data:
            print(f"  - {name}")
        if args.strict:
            print("Pre-populate the test-data directory above, or rerun with --allow-downloads.")
            return 2
        print("Some unicode conformance tests may be unavailable.")
        print()
    else:
        validate_test_data_inputs(test_data_dir)

    if args.check_inputs:
        print("Unicode inputs are ready.")
        return 0

    # Generate all tables
    gen_categories(cache_dir, out_dir)
    gen_combining(cache_dir, out_dir)
    gen_bidi_class(cache_dir, out_dir)
    gen_bidi_brackets(cache_dir, out_dir)
    gen_scripts(cache_dir, out_dir)
    gen_script_extensions(cache_dir, out_dir)
    gen_blocks(cache_dir, out_dir)
    gen_eaw(cache_dir, out_dir)
    gen_linebreak(cache_dir, out_dir)
    gen_grapheme(cache_dir, out_dir)
    gen_wordbreak(cache_dir, out_dir)
    gen_sentbreak(cache_dir, out_dir)
    gen_joining(cache_dir, out_dir)
    gen_age(cache_dir, out_dir)
    gen_proplist(cache_dir, out_dir)
    gen_decomposition(cache_dir, out_dir)
    gen_composition(cache_dir, out_dir)
    gen_normprops(cache_dir, out_dir)
    gen_casemap(cache_dir, out_dir)
    gen_confusables(cache_dir, out_dir)
    gen_identifiers(cache_dir, out_dir)
    gen_idna(cache_dir, out_dir)
    gen_names(cache_dir, out_dir)
    gen_collation(cache_dir, out_dir)
    conformance = gen_conformance(test_data_dir, conformance_dir,
                                  conformance_program, args.strict)
    gen_versions(out_dir, versions)

    validate_generated_outputs(out_dir, conformance_dir, conformance_program,
                               conformance)

    print("\nAll tables generated successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
