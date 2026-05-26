#include <stdint.h>

#include "n00b.h"
#include "core/gc_map.h"
#include "core/static_objects.h"

#define STATIC_SECTION_EXTRA_TINFO UINT64_C(0x5354434558540001)
#define STATIC_SECTION_EXTRA_ID    UINT64_C(0x535443450001)

int n00b_test_static_section_extra_value = 41;

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_extra_desc,
                                  n00b_test_static_section_extra_value,
                                  STATIC_SECTION_EXTRA_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_NONE,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_EXTRA_ID);

void *
n00b_test_static_section_extra_addr(void)
{
    return &n00b_test_static_section_extra_value;
}
