import sys


def run(rounds, tail):
    total = 0
    for _ in range(rounds):
        joined = "x" + tail
        total += len(joined.encode())
    return total


sys.exit(0 if run(12000000 * len(sys.argv),
                  "a piece of text long enough to need the heap") == 540000000
         else 1)
