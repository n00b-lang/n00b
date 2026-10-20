import sys

src, dst, sym = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()

with open(dst, "w") as out:
    out.write("#pragma once\n\n")
    out.write("static const char %s[] =\n" % sym)
    for line in data.split(b"\n"):
        esc = line.decode("utf-8").replace("\\", "\\\\").replace('"', '\\"')
        out.write('    "%s\\n"\n' % esc)
    out.write("    ;\n")
