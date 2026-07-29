#include "thermal_policy.h"

sg_thermal_mode_t sg_thermal_next_mode(
    sg_thermal_mode_t current_mode,
    double temperature_c,
    double high_temperature_c,
    double recovery_temperature_c) {
    if (current_mode == SG_THERMAL_HOT) {
        return temperature_c <= recovery_temperature_c
            ? SG_THERMAL_NORMAL
            : SG_THERMAL_HOT;
    }

    return temperature_c >= high_temperature_c
        ? SG_THERMAL_HOT
        : SG_THERMAL_NORMAL;
}

const char *sg_thermal_mode_name(sg_thermal_mode_t mode) {
    return mode == SG_THERMAL_HOT ? "HOT" : "NORMAL";
}
