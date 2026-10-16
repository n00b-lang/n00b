#include "trip.h"

int
trip_client_run(void)
{
    if (trip_entry(0) != 0) {
        return 1;
    }
    trip_entry(1);
    return 2;
}
