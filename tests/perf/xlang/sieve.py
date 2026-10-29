import sys


def count_primes(limit):
    composite = [False] * (limit + 1)
    count = 0
    n = 2
    while n <= limit:
        if not composite[n]:
            count += 1
            multiple = n * n
            while multiple <= limit:
                composite[multiple] = True
                multiple += n
        n += 1
    return count


total = 0
for _ in range(6 * len(sys.argv)):
    total += count_primes(2000000)
sys.exit(0 if total == 893598 else 1)
