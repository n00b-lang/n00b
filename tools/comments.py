import ast
import io
import pathlib
import re
import tokenize

C_SUFFIXES = {".c", ".h", ".def"}
HASH_SUFFIXES = {".sh": "shell", ".bash": "shell", ".py": "python", ".mk": "make",
                 ".yml": "yaml", ".yaml": "yaml", ".bnf": "grammar", ".lex": "grammar",
                 ".tests": "manifest", ".n": "n00b"}
NAMES = {"Makefile": "make", "GNUmakefile": "make", "build.sh": "shell", "budget": "manifest",
         "Dockerfile": "shell"}

DIRECTIVE = re.compile(
    r"(?:#|//|/\*)\s*(?:shellcheck\s|pragma\b|noqa\b|type:\s*ignore|NOLINT|"
    r"clang-format\s+(?:on|off)|-\*-|fmt:\s*(?:on|off|skip))")

C_LEX = re.compile(r'"(?:\\.|[^"\\\n])*"?|\'(?:\\.|[^\'\\\n])*\'?|//(?:\\\n|[^\n])*'
                   r'|/\*[\s\S]*?(?:\*/|\Z)|^[ \t]*#[ \t]*if[ \t]+0\b[^\n]*', re.M)
SHELL_WORD_START = set(" \t\n;|&()")


def language(path):
    path = pathlib.PurePath(path)
    if path.suffix in C_SUFFIXES:
        return "c"
    if path.name in NAMES:
        return NAMES[path.name]
    if path.name.endswith(".Dockerfile"):
        return "shell"
    return HASH_SUFFIXES.get(path.suffix)


def _c_spans(text):
    for m in C_LEX.finditer(text):
        token = m.group(0).lstrip()
        if token.startswith(("//", "/*", "#")):
            yield m.start() + len(m.group(0)) - len(token), m.end()


def _grammar_spans(text):
    single = double = False
    i = 0
    while i < len(text):
        c = text[i]
        if c == "'" and not double:
            single = not single
        elif c == '"' and not single:
            double = not double
        if c == "#" and not single and not double:
            end = text.find("\n", i)
            end = len(text) if end < 0 else end
            yield i, end
            i = end
            continue
        i += 1


def _python_spans(text):
    lines = text.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))

    def at(row, col):
        return offsets[row - 1] + col

    spans = []
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            if token.type == tokenize.COMMENT:
                spans.append((at(*token.start), at(*token.end)))
        tree = ast.parse(text)
    except (SyntaxError, tokenize.TokenError):
        return spans
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            body = node.body
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
                    and isinstance(body[0].value.value, str):
                doc = body[0]
                spans.append((at(doc.lineno, doc.col_offset), at(doc.end_lineno, doc.end_col_offset)))
    return sorted(spans)


class _Shell:
    def __init__(self, text):
        self.text = text
        self.spans = []
        self.heredocs = []

    def run(self):
        self.code(0, None)
        return self.spans

    def word_start(self, i):
        return i == 0 or self.text[i - 1] in SHELL_WORD_START

    def newline(self, i):
        text = self.text
        while self.heredocs:
            word, tabs = self.heredocs.pop(0)
            while i < len(text):
                end = text.find("\n", i)
                end = len(text) if end < 0 else end + 1
                line = text[i:end].rstrip("\n")
                i = end
                if (line.lstrip("\t") if tabs else line) == word:
                    break
        return i

    def code(self, i, closer):
        text = self.text
        depth = 0
        while i < len(text):
            c = text[i]
            if c == "\\":
                i += 2
            elif c == "'":
                end = text.find("'", i + 1)
                i = len(text) if end < 0 else end + 1
            elif c == '"':
                i = self.double(i + 1)
            elif c == "`":
                end = text.find("`", i + 1)
                i = len(text) if end < 0 else end + 1
            elif c == "$" and text.startswith("$(", i):
                i = self.code(i + 2, ")")
            elif c == "$" and text.startswith("${", i):
                end = text.find("}", i)
                i = len(text) if end < 0 else end + 1
            elif c == "#" and self.word_start(i):
                end = text.find("\n", i)
                end = len(text) if end < 0 else end
                self.spans.append((i, end))
                i = end
            elif c == "<" and text.startswith("<<", i) and not text.startswith("<<<", i):
                m = re.compile(r"<<(-?)[ \t]*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\2").match(text, i)
                if m:
                    self.heredocs.append((m.group(3), bool(m.group(1))))
                    i = m.end()
                else:
                    i += 2
            elif c == "\n":
                i = self.newline(i + 1)
            elif c == "(":
                depth += 1
                i += 1
            elif c == ")" and closer and depth == 0:
                return i + 1
            elif c == ")":
                depth = max(0, depth - 1)
                i += 1
            else:
                i += 1
        return i

    def double(self, i):
        text = self.text
        while i < len(text):
            c = text[i]
            if c == "\\":
                i += 2
            elif c == '"':
                return i + 1
            elif text.startswith("$(", i):
                i = self.code(i + 2, ")")
            elif c == "`":
                end = text.find("`", i + 1)
                i = len(text) if end < 0 else end + 1
            else:
                i += 1
        return i


def _shell_spans(text):
    return _Shell(text).run()


def _make_spans(text):
    spans = []
    offset = 0
    for line in text.splitlines(keepends=True):
        if line.startswith("\t"):
            spans += [(offset + a, offset + b) for a, b in _shell_spans(line)]
        else:
            m = re.search(r"(?<!\\)#", line)
            if m:
                spans.append((offset + m.start(), offset + len(line.rstrip("\n"))))
        offset += len(line)
    return spans


def _yaml_spans(text):
    spans = []
    offset = 0
    for line in text.splitlines(keepends=True):
        quote = None
        for i, c in enumerate(line):
            if quote:
                if c == quote:
                    quote = None
            elif c in "'\"" and (i == 0 or line[i - 1] in " \t:[{,-"):
                quote = c
            elif c == "#" and (i == 0 or line[i - 1] in " \t"):
                spans.append((offset + i, offset + len(line.rstrip("\n"))))
                break
        offset += len(line)
    return spans


def _manifest_spans(text):
    return [(m.start(), m.end()) for m in re.finditer(r"^#[^\n]*", text, re.M)]


SCANNERS = {"c": _c_spans, "grammar": _grammar_spans,
            "python": _python_spans, "shell": _shell_spans, "make": _make_spans,
            "yaml": _yaml_spans, "manifest": _manifest_spans}


def is_directive(text, start, end):
    body = text[start:end]
    return (start == 0 and body.startswith("#!")) or bool(DIRECTIVE.match(body))


def comment_spans(text, lang, directives=False):
    return [(a, b) for a, b in SCANNERS[lang](text)
            if b > a and (directives or not is_directive(text, a, b))]


def comments(text, lang):
    return [(text.count("\n", 0, a) + 1, text[a:b]) for a, b in comment_spans(text, lang)]


def blank(text, lang):
    out = []
    last = 0
    for a, b in comment_spans(text, lang, directives=True):
        out.append(text[last:a])
        out.append(re.sub(r"[^\n]", " ", text[a:b]))
        last = b
    out.append(text[last:])
    return "".join(out)


def strip(text, lang):
    out = []
    last = 0
    for a, b in comment_spans(text, lang):
        out.append(text[last:a])
        out.append("\n" * text.count("\n", a, b))
        last = b
    out.append(text[last:])
    original = text.splitlines()
    stripped = "".join(out).splitlines()
    kept = [new.rstrip() if new != old else new for old, new in zip(original, stripped)
            if new.strip() or not old.strip()]
    return "\n".join(kept) + ("\n" if text.endswith("\n") else "")
