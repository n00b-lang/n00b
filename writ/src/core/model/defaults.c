#include "model.h"

static const uint32_t default_values[] = {
#define WRIT_DEFAULT(id, value, section) value,
#include "defaults.def"
#undef WRIT_DEFAULT
};

uint32_t
writ_default(writ_default_t id)
{
    return default_values[id];
}
