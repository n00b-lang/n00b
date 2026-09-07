# a comment
import os

def greet(name: str = "world") -> str:
    """Doc."""
    n = 0x1f + 1_000 + 2.5e3
    if name:
        return f"hello {name!r} and {n}"
    return 'bye'
