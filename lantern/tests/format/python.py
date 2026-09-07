import os
def greet(name):
    n = 1
    if name:
        return name
    return n
def outer():
    def inner():
        return 2
    return inner
