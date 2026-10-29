import sys


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


sys.exit(0 if fib(34 + len(sys.argv)) == 9227465 else 1)
