import os

def f(a, b=(1,
            2)):
    # comment only line
    if a:

        return [x
                for x in b]
    elif b:
        x = 1 + \
            2
        while x:
            x -= 1
    return 'done'

class C:
    pass
y = f(1)
