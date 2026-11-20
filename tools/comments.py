import ast
import ctypes
import functools
import glob
import io
import os
import pathlib
import re
import shutil
import subprocess
import tokenize

LANGS = {".c": "c", ".h": "c", ".def": "c", ".py": "python", ".sh": "shell", ".tests": "shell",
         ".mk": "make", ".yml": "yaml", ".yaml": "yaml", ".bnf": "grammar", ".lex": "grammar",
         ".writ": "writ", ".n": "n00b"}
NAMES = {"Makefile": "make", "build.sh": "shell", "budget": "shell", "Dockerfile": "shell"}
HASH = {"shell": ("'\"", " \"", True, True, True), "make": ("'\"", " \"", True, False, False),
        "yaml": ("'\"", "", True, False, False), "grammar": ("'\"", "", False, False, True),
        "writ": ("\"", "\"", False, False, False)}

DIRECTIVE = re.compile(
    r"(?:#|//|/\*)\s*(?:shellcheck\s|pragma\b|noqa\b|type:\s*ignore|NOLINT|"
    r"clang-format\s+(?:on|off)|-\*-|fmt:\s*(?:on|off|skip))")
HEREDOC = re.compile(r"<<(-?)[ \t]*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\2")
ROOT = pathlib.Path(__file__).resolve().parent.parent


class LexError(Exception):
    pass


def language(path):
    path = pathlib.PurePath(path)
    if path.name in NAMES or path.name.endswith(".Dockerfile"):
        return NAMES.get(path.name, "shell")
    return LANGS.get(path.suffix)


def _hash_spans(text, lang):
    quotes, escapes, word_start, heredocs, multiline = HASH[lang]
    spans, quote, pending, offset = [], None, [], 0
    for line in text.splitlines(keepends=True):
        body, quote = line.rstrip("\r\n"), quote if multiline else None
        if pending:
            if (body.lstrip("\t") if pending[0][0] else body) == pending[0][1]:
                pending.pop(0)
            offset += len(line)
            continue
        i = 0
        while i < len(body):
            c = body[i]
            if c == "\\" and (quote or " ") in escapes:
                i += 2
                continue
            if quote:
                quote = None if c == quote else quote
            elif c in quotes:
                quote = c
            elif c == "#" and (not word_start or i == 0 or body[i - 1] in " \t;|&()"):
                spans.append((offset + i, offset + len(body)))
                break
            elif heredocs and body.startswith("<<", i) and not body.startswith("<<<", i):
                m = HEREDOC.match(body, i)
                if m:
                    pending.append((m.group(1) == "-", m.group(3)))
                    i = m.end()
                    continue
            i += 1
        offset += len(line)
    return spans


def _python_spans(text):
    starts = [0]
    for line in text.splitlines(keepends=True):
        starts.append(starts[-1] + len(line))
    try:
        spans = [(starts[t.start[0] - 1] + t.start[1], starts[t.end[0] - 1] + t.end[1])
                 for t in tokenize.generate_tokens(io.StringIO(text).readline)
                 if t.type == tokenize.COMMENT]
        tree = ast.parse(text)
    except (SyntaxError, tokenize.TokenError) as err:
        raise LexError(f"python: {err}")
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) or (
                isinstance(node, ast.Module) and "__doc__" not in text):
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
                    and isinstance(body[0].value.value, str):
                doc, lines = body[0], text.splitlines(keepends=True)
                at = [starts[row - 1] + len(lines[row - 1].encode()[:col].decode(errors="ignore"))
                      for row, col in ((doc.lineno, doc.col_offset),
                                       (doc.end_lineno, doc.end_col_offset))]
                spans.append(tuple(at))
    return sorted(spans)


class _Loc(ctypes.Structure):
    _fields_ = [("p", ctypes.c_void_p * 2), ("d", ctypes.c_uint)]


class _Range(ctypes.Structure):
    _fields_ = [("p", ctypes.c_void_p * 2), ("b", ctypes.c_uint), ("e", ctypes.c_uint)]


class _Token(ctypes.Structure):
    _fields_ = [("d", ctypes.c_uint * 4), ("p", ctypes.c_void_p)]


class _Unsaved(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("data", ctypes.c_char_p), ("size", ctypes.c_ulong)]


def _llvm_dirs():
    config = pathlib.Path(os.environ.get("BUILD") or ROOT / "build") / "config.mk"
    if config.exists():
        found = dict(re.findall(r"^(LLVM_LIBDIR|LLVM_BINDIR) := (.*)$", config.read_text(), re.M))
        yield from found.values()
    if os.environ.get("LLVM_CONFIG"):
        for flag in ("--libdir", "--bindir"):
            yield subprocess.run([os.environ["LLVM_CONFIG"], flag], capture_output=True,
                                 text=True).stdout.strip()


@functools.lru_cache(maxsize=None)
def _libclang():
    for directory in _llvm_dirs():
        if not os.path.isdir(directory) and shutil.which("cygpath"):
            directory = subprocess.run(["cygpath", "-u", directory], capture_output=True,
                                       text=True).stdout.strip()
        for path in sorted(glob.glob(os.path.join(directory, "libclang[.-]*"))):
            try:
                lib = ctypes.CDLL(path)
                lib.clang_createIndex
            except (OSError, AttributeError):
                continue
            p, u = ctypes.c_void_p, ctypes.c_uint
            for name, res, args in (
                    ("createIndex", p, [ctypes.c_int, ctypes.c_int]),
                    ("parseTranslationUnit", p, [p, ctypes.c_char_p, p, ctypes.c_int,
                                                 ctypes.POINTER(_Unsaved), u, u]),
                    ("getFile", p, [p, ctypes.c_char_p]),
                    ("getLocationForOffset", _Loc, [p, p, u]),
                    ("getRange", _Range, [_Loc, _Loc]),
                    ("tokenize", None, [p, _Range, ctypes.POINTER(ctypes.POINTER(_Token)),
                                        ctypes.POINTER(u)]),
                    ("getTokenKind", u, [_Token]),
                    ("getTokenExtent", _Range, [p, _Token]),
                    ("getRangeStart", _Loc, [_Range]),
                    ("getRangeEnd", _Loc, [_Range]),
                    ("getFileLocation", None, [_Loc, p, p, p, ctypes.POINTER(u)]),
                    ("disposeTokens", None, [p, ctypes.POINTER(_Token), u]),
                    ("disposeTranslationUnit", None, [p])):
                getattr(lib, "clang_" + name).restype = res
                getattr(lib, "clang_" + name).argtypes = args
            return lib, lib.clang_createIndex(0, 0)
    raise LexError("libclang not found: build first, or set BUILD or LLVM_CONFIG")


def _c_spans(text):
    if "//" not in text and "/*" not in text:
        return []
    lib, index = _libclang()
    data, name = text.encode("utf-8", "surrogateescape"), b"source.c"
    tu = lib.clang_parseTranslationUnit(index, name, None, 0,
                                        ctypes.byref(_Unsaved(name, data, len(data))), 1, 0x400)
    if not tu:
        raise LexError("libclang could not read the file")
    file = lib.clang_getFile(tu, name)
    whole = lib.clang_getRange(lib.clang_getLocationForOffset(tu, file, 0),
                               lib.clang_getLocationForOffset(tu, file, len(data)))
    tokens, count, spans = ctypes.POINTER(_Token)(), ctypes.c_uint(), []
    lib.clang_tokenize(tu, whole, ctypes.byref(tokens), ctypes.byref(count))
    for k in range(count.value):
        if lib.clang_getTokenKind(tokens[k]) == 4:
            extent, ends = lib.clang_getTokenExtent(tu, tokens[k]), []
            for loc in (lib.clang_getRangeStart(extent), lib.clang_getRangeEnd(extent)):
                at = ctypes.c_uint()
                lib.clang_getFileLocation(loc, None, None, None, ctypes.byref(at))
                ends.append(len(data[:at.value].decode("utf-8", "surrogateescape")))
            spans.append(tuple(ends))
    lib.clang_disposeTokens(tu, tokens, count)
    lib.clang_disposeTranslationUnit(tu)
    return spans


@functools.lru_cache(maxsize=4096)
def _scan(text, lang):
    if lang == "c":
        return tuple(_c_spans(text))
    if lang == "python":
        return tuple(_python_spans(text))
    return tuple(_hash_spans(text, lang))


def comment_spans(text, lang, directives=False):
    return [(a, b) for a, b in _scan(text, lang) if b > a and (directives or not (
        (a == 0 and text.startswith("#!")) or DIRECTIVE.match(text, a)))]


def comments(text, lang):
    try:
        return [(text.count("\n", 0, a) + 1, text[a:b]) for a, b in comment_spans(text, lang)]
    except LexError as err:
        return [(1, f"cannot be lexed: {err}")]


def blank(text, lang):
    try:
        spans = comment_spans(text, lang, directives=True)
    except LexError:
        return text
    out, last = [], 0
    for a, b in spans:
        out += [text[last:a], re.sub(r"[^\n]", " ", text[a:b])]
        last = b
    return "".join(out) + text[last:]


def strip(text, lang):
    out, marks, last = [], [], 0
    for a, b in comment_spans(text, lang):
        block, before, after = text.startswith("/*", a), text[a - 1:a], text[b:b + 1]
        if block and before in (" ", "\t") and after == " ":
            b += 1
            after = text[b:b + 1]
        out.append(text[last:a])
        if block and before.strip() and after.strip():
            out.append(" ")
        marks.append(sum(map(len, out)))
        last = b
    joined = "".join(out) + text[last:]
    result, at, dropped, last_blank, kept = [], 0, False, True, 0
    for line in joined.splitlines(keepends=True):
        end = at + len(line.rstrip("\n"))
        touched = any(at <= mark <= end for mark in marks)
        body = line.rstrip("\n").rstrip(" \t") if touched else line.rstrip("\n")
        blank_line = not body.strip(" \t\r\f\v")
        if touched and blank_line:
            dropped = True
        elif not (blank_line and dropped and last_blank):
            result.append(body + line[len(line.rstrip("\n")):])
            last_blank = blank_line
            if not blank_line:
                dropped, kept = False, len(result)
        at += len(line)
    new = "".join(result[:kept] if dropped else result)
    if lang == "python":
        ast.parse(new)
    return new
