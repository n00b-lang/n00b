#include "test_unicode_helpers.h"
#include "text/strings/fptostr.h"

#include <math.h>
#include <stdlib.h>
#include <float.h>

// Helper: convert with fptostr and NUL-terminate for comparison.
static int
fptostr_buf(double d, char *buf)
{
    int n  = n00b_fptostr(d, buf);
    buf[n] = '\0';
    return n;
}

// ===================================================================
// Basic values
// ===================================================================

TEST(test_zero)
{
    char buf[24];
    fptostr_buf(0.0, buf);
    ASSERT_STR_EQ(buf, "0");
}

TEST(test_negative_zero)
{
    char buf[24];
    fptostr_buf(-0.0, buf);
    ASSERT_STR_EQ(buf, "-0");
}

TEST(test_one)
{
    char buf[24];
    fptostr_buf(1.0, buf);
    ASSERT_STR_EQ(buf, "1");
}

TEST(test_negative_one)
{
    char buf[24];
    fptostr_buf(-1.0, buf);
    ASSERT_STR_EQ(buf, "-1");
}

TEST(test_positive_integer)
{
    char buf[24];
    fptostr_buf(42.0, buf);
    ASSERT_STR_EQ(buf, "42");
}

TEST(test_negative_integer)
{
    char buf[24];
    fptostr_buf(-100.0, buf);
    ASSERT_STR_EQ(buf, "-100");
}

// ===================================================================
// Decimal values
// ===================================================================

TEST(test_point_one)
{
    char buf[24];
    fptostr_buf(0.1, buf);
    ASSERT_STR_EQ(buf, "0.1");
}

TEST(test_pi)
{
    char buf[24];
    fptostr_buf(3.14159, buf);
    // Grisu2 should produce "3.14159"
    double back = strtod(buf, nullptr);
    ASSERT(back == 3.14159);
}

TEST(test_simple_decimal)
{
    char buf[24];
    fptostr_buf(1.5, buf);
    ASSERT_STR_EQ(buf, "1.5");
}

// ===================================================================
// Special values
// ===================================================================

TEST(test_nan)
{
    char buf[24];
    fptostr_buf(NAN, buf);
    // NAN may have sign bit set on some platforms.
    ASSERT(strcmp(buf, "nan") == 0 || strcmp(buf, "-nan") == 0);
}

TEST(test_positive_inf)
{
    char buf[24];
    fptostr_buf(INFINITY, buf);
    ASSERT_STR_EQ(buf, "inf");
}

TEST(test_negative_inf)
{
    char buf[24];
    fptostr_buf(-INFINITY, buf);
    ASSERT_STR_EQ(buf, "-inf");
}

// ===================================================================
// Extreme values
// ===================================================================

TEST(test_very_small)
{
    char   buf[24];
    double d = 1e-300;
    fptostr_buf(d, buf);
    double back = strtod(buf, nullptr);
    ASSERT(back == d);
}

TEST(test_very_large)
{
    char   buf[24];
    double d = 1e+300;
    fptostr_buf(d, buf);
    double back = strtod(buf, nullptr);
    ASSERT(back == d);
}

TEST(test_dbl_min)
{
    char   buf[24];
    double d = DBL_MIN;
    fptostr_buf(d, buf);
    double back = strtod(buf, nullptr);
    ASSERT(back == d);
}

TEST(test_dbl_max)
{
    char   buf[24];
    double d = DBL_MAX;
    fptostr_buf(d, buf);
    double back = strtod(buf, nullptr);
    ASSERT(back == d);
}

// ===================================================================
// Roundtrip correctness
// ===================================================================

TEST(test_roundtrip_various)
{
    double vals[] = {0.1, 0.2, 0.3, 1.23456789, 1e10, 1e-10,
                     123456789.123456789, -0.00001};
    int n = sizeof(vals) / sizeof(vals[0]);

    for (int i = 0; i < n; i++) {
        char   buf[24];
        fptostr_buf(vals[i], buf);
        double back = strtod(buf, nullptr);
        ASSERT(back == vals[i]);
    }
}

static void run_tests(void)
{
    RUN_TEST(test_zero);
    RUN_TEST(test_negative_zero);
    RUN_TEST(test_one);
    RUN_TEST(test_negative_one);
    RUN_TEST(test_positive_integer);
    RUN_TEST(test_negative_integer);
    RUN_TEST(test_point_one);
    RUN_TEST(test_pi);
    RUN_TEST(test_simple_decimal);
    RUN_TEST(test_nan);
    RUN_TEST(test_positive_inf);
    RUN_TEST(test_negative_inf);
    RUN_TEST(test_very_small);
    RUN_TEST(test_very_large);
    RUN_TEST(test_dbl_min);
    RUN_TEST(test_dbl_max);
    RUN_TEST(test_roundtrip_various);
}

TEST_MAIN()
