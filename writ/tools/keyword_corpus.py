import re
import sys

grammar_path, output_path = sys.argv[1], sys.argv[2]

with open(grammar_path, encoding="utf-8") as grammar:
    words = sorted(set(re.findall(r'%"([a-z_][a-z_0-9]*)"', grammar.read())))

lines = ["library library"]
for word in words:
    lines.append(f"fn f_{word}({word}: {word}, len: *[{word}; cap {word}, len {word}.out], flags: c_int) -> {word}")
    lines.append(f"  error {word} != {word} via {word}({word})")
    lines.append(f"const {word}: {word} = {word}")

with open(output_path, "w", encoding="utf-8") as out:
    out.write("\n".join(lines) + "\n")
