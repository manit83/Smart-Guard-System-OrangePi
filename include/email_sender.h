#ifndef SMART_GUARD_EMAIL_SENDER_H
#define SMART_GUARD_EMAIL_SENDER_H

#include "smart_guard_notifier.h"

#include <stddef.h>

int sg_email_global_init(char *error, size_t error_size);
void sg_email_global_cleanup(void);

int sg_send_alert_email(
    const sg_config_t *config,
    const sg_vision_state_t *state,
    double temperature_c,
    char *error,
    size_t error_size);

int sg_send_tamper_email(
    const sg_config_t *config,
    const char *timestamp,
    double temperature_c,
    unsigned long stale_seconds,
    char *error,
    size_t error_size);

int sg_send_thermal_email(
    const sg_config_t *config,
    const char *timestamp,
    double temperature_c,
    double high_temperature_c,
    bool hot_profile_enabled,
    int camera_width,
    int camera_height,
    int camera_input_fps,
    double vision_output_fps,
    char *error,
    size_t error_size);

#endif
