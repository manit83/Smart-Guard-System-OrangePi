#include "thermal_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(sg_thermal_next_mode(
        SG_THERMAL_NORMAL, 69.9, 70.0, 60.0) == SG_THERMAL_NORMAL);
    assert(sg_thermal_next_mode(
        SG_THERMAL_NORMAL, 70.0, 70.0, 60.0) == SG_THERMAL_HOT);
    assert(sg_thermal_next_mode(
        SG_THERMAL_HOT, 65.0, 70.0, 60.0) == SG_THERMAL_HOT);
    assert(sg_thermal_next_mode(
        SG_THERMAL_HOT, 60.0, 70.0, 60.0) == SG_THERMAL_NORMAL);
    assert(strcmp(sg_thermal_mode_name(SG_THERMAL_NORMAL), "NORMAL") == 0);
    assert(strcmp(sg_thermal_mode_name(SG_THERMAL_HOT), "HOT") == 0);

    puts("thermal policy tests passed");
    return 0;
}
