class E {
    long a = 1_000L + 0x1F_FFl + 0b1010 + 0;
    double b = 1.5e-3 + .5 + 5. + 0x1.8p3 + 3e10d + 2f + 7D;
    char c = '\n'; char d = '\''; String s = "a\"b\\c\td";
    String t = """
        text block "quoted" \n
        """;
    int x = a >>> 2 >>= 1 != 3 && b <= c ? d : e;
    /* block * comment */ // line comment
    int y = 1..2;
    var z = obj.field.method();
}
