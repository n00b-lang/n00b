class E {
    ulong a = 10UL + 0xFF_FFu + 0b_1010L + 1_0lu;
    decimal m = 1.5m + 2.0f + .25d + 3e5 + 4E-2F;
    string v = @"C:\path\
second line";
    string i = $"value {x}";
    string r = """raw "text" here""";
    char c = 'c'; char n = '\'';
    int x = a ?? b?.c ?? d << 2 <<= 3 => e;
    /* block */ // line
}
