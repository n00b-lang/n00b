import re
import sys

grammar_path, output_path = sys.argv[1], sys.argv[2]

with open(grammar_path, encoding="utf-8") as grammar:
    words = sorted(set(re.findall(r'%"([a-z_][a-z_0-9]*)"', grammar.read())))

entries = "".join(f'    CORE_STRING_STATIC("{word}"),\n' for word in words)
text = ("#pragma once\n"
        '#include "lib/string.h"\n'
        "static const core_string_t writ_soft_keywords[] = {\n"
        f"{entries}"
        "    {0},\n"
        "};\n")

try:
    with open(output_path, encoding="utf-8") as existing:
        unchanged = existing.read() == text
except FileNotFoundError:
    unchanged = False
if not unchanged:
    with open(output_path, "w", encoding="utf-8") as out:
        out.write(text)
