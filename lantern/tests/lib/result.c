#include "lib/result.h"
#include "harness.h"

core_result_decl(int_result_t, int, int);

int
main(void)
{
    int_result_t success = core_result_ok(int_result_t, 27);
    int_result_t failure = core_result_err(int_result_t, 5);

    check("result success stores and extracts its value",
          success.is_ok && core_result_get_ok(success) == 27);
    check("result failure stores and extracts its error",
          !failure.is_ok && core_result_get_err(failure) == 5);
    return 0;
}
