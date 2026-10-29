import sys


def run(rounds, size):
    total = 0
    for _ in range(rounds):
        items = []
        table = {}
        i = 0
        while i < size:
            items.append(i)
            table[i] = i * 2
            i += 1
        for item in items:
            found = table.get(item)
            if found is not None:
                total += found
    return total


sys.exit(0 if run(50 * len(sys.argv), 200000) == 1999990000000 else 1)
