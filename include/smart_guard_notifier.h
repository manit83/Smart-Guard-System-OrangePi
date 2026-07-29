#ifndef SMART_GUARD_NOTIFIER_H
#define SMART_GUARD_NOTIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SG_PATH_SIZE 512
#define SG_TEXT_SIZE 256
#define SG_TIMESTAMP_SIZE 64
#define SG_ERROR_SIZE 512
#define SG_PAYLOAD_SIZE 768

typedef struct {
    char student_id[SG_TEXT_SIZE];
    char vision_state_path[SG_PATH_SIZE];
    char latest_frame_path[SG_PATH_SIZE];
    char cpu_temp_path[SG_PATH_SIZE];
    char last_email_state_path[SG_PATH_SIZE];
    char guard_state_path[SG_PATH_SIZE];
    char last_guard_alert_state_path[SG_PATH_SIZE];
    char history_db_path[SG_PATH_SIZE];
    char last_tamper_state_path[SG_PATH_SIZE];
    int poll_interval_ms;
    bool guard_default_enabled;
    int guard_alert_debounce_sec;
    int history_capacity;
    int history_event_rearm_sec;

    bool email_enabled;
    char email_to[SG_TEXT_SIZE];
    char smtp_host[SG_TEXT_SIZE];
    int smtp_port;
    char smtp_security[32];
    char smtp_username[SG_TEXT_SIZE];
    char smtp_from[SG_TEXT_SIZE];
    char smtp_password_file[SG_PATH_SIZE];
    char smtp_ca_file[SG_PATH_SIZE];
    char email_subject_prefix[SG_TEXT_SIZE];
    int email_debounce_sec;

    bool watchdog_email_enabled;
    bool watchdog_restart_enabled;
    int watchdog_frame_timeout_sec;
    int watchdog_poll_interval_ms;
    int watchdog_incident_debounce_sec;
    char watchdog_restart_service[SG_TEXT_SIZE];

    bool mqtt_enabled;
    char mqtt_broker_host[SG_TEXT_SIZE];
    int mqtt_broker_port;
    char mqtt_username[SG_TEXT_SIZE];
    char mqtt_password_file[SG_PATH_SIZE];
    int mqtt_keepalive_sec;
    int mqtt_telemetry_interval_sec;
    int mqtt_persons_interval_sec;
} sg_config_t;

typedef struct {
    int persons;
    char timestamp[SG_TIMESTAMP_SIZE];
} sg_vision_state_t;

int sg_load_config(sg_config_t *config, char *error, size_t error_size);

int sg_parse_vision_state_json(
    const char *json,
    sg_vision_state_t *state,
    char *error,
    size_t error_size);

int sg_read_vision_state(
    const char *path,
    sg_vision_state_t *state,
    char *error,
    size_t error_size);

int sg_read_cpu_temperature(
    const char *configured_path,
    double *temperature_c,
    char *error,
    size_t error_size);

int sg_build_event_payload(
    const sg_config_t *config,
    const sg_vision_state_t *state,
    double temperature_c,
    char *payload,
    size_t payload_size);

int sg_format_timestamp(
    time_t timestamp,
    char *buffer,
    size_t buffer_size);

bool sg_email_due(time_t now, time_t last_attempt, int debounce_sec);

int sg_read_epoch_file(
    const char *path,
    time_t *value,
    char *error,
    size_t error_size);

int sg_atomic_write_epoch(
    const char *path,
    time_t value,
    char *error,
    size_t error_size);

int sg_read_secret(
    const char *path,
    char *buffer,
    size_t buffer_size,
    char *error,
    size_t error_size);

#endif
