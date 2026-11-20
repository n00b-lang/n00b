import collections
import hashlib
import pathlib
import re
import sys

import source_tree

WINDOW = 80
STATUSES = ("pending", "false-positive")
C_ROOTS = ("compiler/", "lantern/src/", "lantern/include/", "runtime/", "abi/", "writ/src/", "writ/include/")
N00B_ROOTS = ("lib/",)
KEYWORDS = {
    "c": "auto break case char const continue default do double else enum extern float for goto if "
         "inline int long register restrict return short signed sizeof static struct switch typedef "
         "union unsigned void volatile while bool true false nullptr NULL",
    "n00b": "alias as assert break bring case cast const continue default elif else enum error export "
            "fail false flagset fn for func function if import in is let module new nil not or out "
            "pass record ref return struct switch trait true try type typecase var variant when where "
            "while with",
}
LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])\'')
NUMBER = re.compile(r"\b\d[\w.]*")
TOKEN = re.compile(r"\w+|\S")
TRIVIAL = {"{", "}", "};", ")", ");", "else", "}else{"}
FILLER = re.compile(
    r"(?:case[^:]*:|default:)+(?:return[^;]*;|break;)?"
    r"|return(?:[^;(]*|I\(@\));|break;"
    r"|(?:static|inline|const|struct|enum|union|unsigned|bool|char|int|long|void|I|\*)+"
    r"|I\([^;{}]*\)\{?"
    r"|(?:const|static|struct|enum|union|unsigned|signed|volatile|bool|char|int|long|short|double|float|void|I|\*)+"
    r"(?:\[[^\]]*\])?(?::0)?;"
    r"|I:[\w\[\]?*]+,?"
    r"|(?:import|bring).*|[^;]*=\{|'I|\}?(?:else)?when.*\{"
    r"|for\((?:[^;]*;){2}[^;{}]*\)\{?"
    r"|(?:const|struct)*I\**I=(?:[\w.\[\]]|->)+;"
    r"|boolI=(?:true|false);"
    r"|(?!if|for|while|switch|return|else|func|fn|var|let)\{*(?:[^;{}]|\{0\})*[,(\[]"
    r"|[\w.\-\[\]=,@]*[)}]+,?|\{?(?:[^;{}]|\{0\})*\},?")
FENCE = re.compile(r"\[(-+)(?=\s)[\s\S]*?(?<=\s)\1\]")
IDENTIFIER = {lang: re.compile(r"(?<!\.)(?<!->)\b(?!(?:%s)\b)[A-Za-z_]\w*" % "|".join(words.split()))
              for lang, words in KEYWORDS.items()}


def scanned(path):
    if "/tests/" in path or "/vendor/" in path or "/generated/" in path or "_tables." in path:
        return None
    if path.endswith((".c", ".h")) and path.startswith(C_ROOTS):
        return "c"
    if path.endswith(".n") and path.startswith(N00B_ROOTS):
        return "n00b"
    return None


def rows(text, lang):
    if lang == "n00b":
        text = FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = LITERAL.sub("@", text)
    text = NUMBER.sub("0", text)
    text = IDENTIFIER[lang].sub("I", text)
    out = []
    for number, line in enumerate(text.split("\n"), 1):
        compact = line.replace(" ", "").replace("\t", "")
        if not compact or compact in TRIVIAL or compact.startswith(("#", "//")):
            continue
        out.append((number, compact, len(TOKEN.findall(line)), bool(FILLER.fullmatch(compact))))
    return out


def windows(path, lines):
    end = 0
    total = 0
    for start in range(len(lines)):
        while end < len(lines) and total < WINDOW:
            total += lines[end][2]
            end += 1
        if total < WINDOW:
            return
        body = [line[1] for line in lines[start:end]]
        filler = sum(line[3] for line in lines[start:end])
        if len(set(body)) * 2 > len(body) and filler * 4 < len(body) * 3:
            key = hashlib.blake2b("\n".join(body).encode(), digest_size=8).hexdigest()
            yield key, (path, lines[start][0], lines[end - 1][0])
        total -= lines[start][2]


def scan(root):
    seen = collections.defaultdict(list)
    for path in source_tree.files(root):
        lang = scanned(path)
        text = None if not lang else source_tree.code(root, path, lang) if lang == "c" \
            else source_tree.read(root, path)
        if text is None:
            continue
        for key, place in windows(path, rows(text, lang)):
            seen[key].append(place)
    clones = {}
    for key, places in seen.items():
        distinct = []
        for place in places:
            if not any(p[0] == place[0] and p[1] <= place[2] and place[1] <= p[2] for p in distinct):
                distinct.append(place)
        if len(distinct) > 1:
            clones[key] = distinct
    return clones


def regions(clones, keys):
    spans = collections.defaultdict(list)
    for key in keys:
        first, *rest = clones[key]
        for other in rest:
            spans[(first[0], other[0])].append((first[1], first[2], other[1], other[2]))
    merged = []
    for (a, b), found in sorted(spans.items()):
        current = None
        for start, end, other_start, other_end in sorted(found):
            if current and start <= current[1] + 1 and current[2] <= other_start <= current[3] + 1:
                current = [current[0], max(current[1], end), current[2], max(current[3], other_end)]
            else:
                if current:
                    merged.append((a, b, current))
                current = [start, end, other_start, other_end]
        if current:
            merged.append((a, b, current))
    return merged


def families(clones):
    parent = {}

    def find(item):
        while parent.setdefault(item, item) != item:
            parent[item] = parent[parent[item]]
            item = parent[item]
        return item

    for key, places in clones.items():
        for place in places:
            parent[find(place)] = find(key)
    places = sorted({place for found in clones.values() for place in found})
    for a, b in zip(places, places[1:]):
        if a[0] == b[0] and b[1] <= a[2] + 1:
            parent[find(a)] = find(b)
    first = {}
    for place in places:
        first.setdefault(find(place), f"{place[0]}:{place[1]}")
    return {key: first[find(key)] for key in clones}


def read_baseline(path):
    baseline = {}
    problems = []
    for number, line in enumerate(path.read_text().splitlines() if path.exists() else [], 1):
        fields = line.split("\t")
        if len(fields) != 4 or fields[2] not in STATUSES or not fields[3].strip() or not fields[1].isdigit():
            problems.append(f"{path.name}:{number}: write key<TAB>count<TAB>pending|false-positive<TAB>family or reason")
            continue
        baseline[fields[0]] = (int(fields[1]), fields[2], fields[3])
    return baseline, problems


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".")
    baseline_path = pathlib.Path(args[1]) if len(args) > 1 else root / "tests/clone-baseline"
    clones = scan(root)
    baseline, problems = read_baseline(baseline_path)
    if "--update" in argv:
        labels = families(clones)
        kept = {key: baseline.get(key, (0, "pending", labels[key])) for key in clones}
        baseline_path.write_text("".join(
            f"{key}\t{len(clones[key])}\t{kept[key][1]}\t"
            f"{kept[key][2] if kept[key][1] != 'pending' else labels[key]}\n"
            for key in sorted(clones, key=lambda k: (labels[k], k))))
        print(f"wrote {len(clones)} cloned windows to {baseline_path}")
        return 0
    if problems:
        print("\n".join(problems))
        return 1
    new = [key for key, places in clones.items() if len(places) > baseline.get(key, (1,))[0]]
    if not new:
        gone = sum(1 for key in baseline if key not in clones)
        pending = len({entry[2] for key, entry in baseline.items() if entry[1] == "pending" and key in clones})
        hint = f"; {gone} baseline entries are gone, so run python3 tools/check_clones.py --update" if gone else ""
        print(f"no new duplication ({pending} duplicate families pending removal{hint})")
        return 0
    for a, b, (start, end, other_start, other_end) in regions(clones, new):
        print(f"{b}:{other_start}-{other_end} duplicates {a}:{start}-{end}")
    print("New duplicated code. Move the shared logic into one function (or extend the existing one) and "
          "call it from both places instead of copying it. If you only edited code that was already "
          "duplicated, consolidate it now. Only a false positive (code that matches in shape but "
          "shares no logic) goes in tests/clone-baseline: run python3 tools/check_clones.py --update "
          "and mark its lines false-positive with a reason.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
