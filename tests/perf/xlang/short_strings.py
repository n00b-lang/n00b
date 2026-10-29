import sys


def run(rounds, tail):
    total = 0
    for _ in range(rounds):
        joined = "x" + tail
        total += len(joined.encode())
    return total


sys.exit(0 if run(12000000 * len(sys.argv), "short tail") == 132000000 else 1)
