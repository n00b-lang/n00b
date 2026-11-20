import importlib
import sys
import time

CHECKS = ("comments", "docs", "clones", "warm_paths")


def main(argv):
    root = argv[0] if argv else "."
    status = 0
    for name in argv[1:] or CHECKS:
        start = time.monotonic()
        failed = importlib.import_module(f"check_{name}").main([root])
        print(f"{'FAIL' if failed else 'ok'}: {name} ({time.monotonic() - start:.2f}s)")
        status |= bool(failed)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
