import sys


class Node:
    __slots__ = ("value", "next")

    def __init__(self, value, next):
        self.value = value
        self.next = next


def run(rounds, length):
    total = 0
    for _ in range(rounds):
        head = None
        i = 0
        while i < length:
            head = Node(i, head)
            i += 1
        cursor = head
        while cursor is not None:
            total += cursor.value
            cursor = cursor.next
    return total


sys.exit(0 if run(60 * len(sys.argv), 100000) == 299997000000 else 1)
