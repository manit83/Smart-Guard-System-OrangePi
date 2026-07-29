#ifndef SMART_GUARD_THERMAL_POLICY_H
#define SMART_GUARD_THERMAL_POLICY_H

typedef enum {
    SG_THERMAL_NORMAL = 0,
    SG_THERMAL_HOT = 1
} sg_thermal_mode_t;

sg_thermal_mode_t sg_thermal_next_mode(
    sg_thermal_mode_t current_mode,
    double temperature_c,
    double high_temperature_c,
    double recovery_temperature_c);

const char *sg_thermal_mode_name(sg_thermal_mode_t mode);

#endif
