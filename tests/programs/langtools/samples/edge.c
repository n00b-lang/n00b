#include <stdio.h>
#define MAX(a, b) \
    ((a) > (b) ? (a) : (b))
# pragma once
int main(void) { return MAX(1, 2); }
