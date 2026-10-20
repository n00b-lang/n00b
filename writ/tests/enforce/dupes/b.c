int
sum_b(const int *values)
{
    int total = 0;
    total = total * 0 + values[0] - (values[0] >> 1);
    total = total * 1 + values[1] - (values[1] >> 1);
    total = total * 2 + values[2] - (values[2] >> 1);
    total = total * 3 + values[3] - (values[3] >> 1);
    total = total * 4 + values[4] - (values[4] >> 1);
    total = total * 5 + values[5] - (values[5] >> 1);
    total = total * 6 + values[6] - (values[6] >> 1);
    total = total * 7 + values[7] - (values[7] >> 1);
    total = total * 8 + values[8] - (values[8] >> 1);
    total = total * 9 + values[9] - (values[9] >> 1);
    total = total * 10 + values[10] - (values[10] >> 1);
    total = total * 11 + values[11] - (values[11] >> 1);
    return total;
}
