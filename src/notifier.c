#define _POSIX_C_SOURCE 200809L

#include "email_sender.h"
#include "guard_state.h"
#include "mqtt_client.h"
#include "smart_guard_notifier.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number) {
    (void) signal_number;
    stop_requested = 1;
}

static double monotonic_seconds(void) {
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0.0;
    }
    return (double) value.tv_sec + (double) value.tv_nsec / 1000000000.0;
}

static void sleep_milliseconds(int milliseconds) {
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        if (stop_requested) {
            break;
        }
    }
}

static bool readable_nonempty_file(const char *path) {
    struct stat information;

    return stat(path, &information) == 0 &&
        S_ISREG(information.st_mode) &&
        information.st_size > 0;
}

static void log_configuration(const sg_config_t *config) {
    fprintf(
        stderr,
        "INFO Smart Guard notifier started for student %s "
        "(email=%s, mqtt=%s)\n",
        config->student_id,
        config->email_enabled ? "enabled" : "disabled",
        config->mqtt_enabled ? "enabled" : "disabled");
    fprintf(
        stderr,
        "INFO Guard state=%s alarm debounce=%ds\n",
        config->guard_state_path,
        config->guard_alert_debounce_sec);
    fprintf(
        stderr,
        "INFO Vision state=%s image=%s\n",
        config->vision_state_path,
        config->latest_frame_path);
    if (config->mqtt_enabled) {
        fprintf(
            stderr,
            "INFO MQTT QoS 1 topics: home/%s/{persons,telemetry,status}, "
            "alarm/%s/home\n",
            config->student_id,
            config->student_id);
    }
}

static int build_alarm_payload(
    const sg_config_t *config,
    const sg_vision_state_t *state,
    double temperature_c,
    char *payload,
    size_t payload_size) {
    int length = snprintf(
        payload,
        payload_size,
        "{\"event\":\"person_detected\",\"guard_mode\":true,"
        "\"student_id\":\"%s\",\"persons\":%d,"
        "\"temperature_c\":%.1f,\"timestamp\":\"%s\"}",
        config->student_id,
        state->persons,
        temperature_c,
        state->timestamp);

    return length < 0 || (size_t) length >= payload_size ? -1 : 0;
}

static void maybe_send_guard_alert(
    const sg_config_t *config,
    sg_mqtt_client_t *mqtt,
    bool guard_enabled,
    const sg_vision_state_t *state,
    double temperature_c,
    time_t *last_alert_attempt) {
    char payload[SG_PAYLOAD_SIZE];
    char error[SG_ERROR_SIZE];
    time_t now;

    if (!guard_enabled || state->persons <= 0 ||
        (!config->email_enabled && !config->mqtt_enabled)) {
        return;
    }

    now = time(NULL);
    if (now == (time_t) -1 ||
        !sg_email_due(
            now,
            *last_alert_attempt,
            config->guard_alert_debounce_sec)) {
        return;
    }

    /*
     * Persist before network I/O. A crash after the SMTP or MQTT server accepts
     * a message cannot create a duplicate alert inside the debounce window.
     */
    if (sg_atomic_write_epoch(
            config->last_guard_alert_state_path,
            now,
            error,
            sizeof(error)) != 0) {
        fprintf(
            stderr,
            "ERROR Guard alert blocked because state cannot be persisted: %s\n",
            error);
        return;
    }
    *last_alert_attempt = now;

    if (config->email_enabled) {
        if (!readable_nonempty_file(config->latest_frame_path)) {
            fprintf(
                stderr,
                "WARNING Guard email skipped because image is unavailable: %s\n",
                config->latest_frame_path);
        } else if (sg_send_alert_email(
                       config,
                       state,
                       temperature_c,
                       error,
                       sizeof(error)) != 0) {
            fprintf(stderr, "ERROR Guard email failed: %s\n", error);
        } else {
            fprintf(
                stderr,
                "INFO Guard email sent: persons=%d timestamp=%s\n",
                state->persons,
                state->timestamp);
        }
    }

    if (config->mqtt_enabled) {
        int result;

        if (build_alarm_payload(
                config,
                state,
                temperature_c,
                payload,
                sizeof(payload)) != 0) {
            fprintf(stderr, "ERROR Emergency MQTT payload is too long\n");
            return;
        }
        result = sg_mqtt_publish_alarm(
            mqtt,
            payload,
            error,
            sizeof(error));
        if (result == 0) {
            fprintf(
                stderr,
                "INFO Emergency MQTT QoS 1 published to alarm/%s/home: %s\n",
                config->student_id,
                payload);
        } else {
            fprintf(
                stderr,
                "%s Emergency MQTT alarm was not published: %s\n",
                result < 0 ? "ERROR" : "WARNING",
                error);
        }
    }
}

static void publish_persons_if_due(
    const sg_config_t *config,
    sg_mqtt_client_t *mqtt,
    const sg_vision_state_t *state,
    double temperature_c,
    double now_monotonic,
    int *last_published_persons,
    double *last_persons_publish) {
    char payload[SG_PAYLOAD_SIZE];
    char error[SG_ERROR_SIZE];
    bool count_changed = state->persons != *last_published_persons;
    bool interval_elapsed =
        *last_persons_publish == 0.0 ||
        now_monotonic - *last_persons_publish >=
            (double) config->mqtt_persons_interval_sec;
    int result;

    if (!config->mqtt_enabled || mqtt == NULL ||
        (!count_changed && !interval_elapsed)) {
        return;
    }
    if (sg_build_event_payload(
            config,
            state,
            temperature_c,
            payload,
            sizeof(payload)) != 0) {
        fprintf(stderr, "ERROR Persons JSON payload is too long\n");
        return;
    }

    result = sg_mqtt_publish_persons(
        mqtt,
        payload,
        error,
        sizeof(error));
    if (result == 0) {
        *last_published_persons = state->persons;
        *last_persons_publish = now_monotonic;
        fprintf(stderr, "INFO MQTT QoS 1 persons published: %s\n", payload);
    } else if (result < 0) {
        fprintf(stderr, "ERROR %s\n", error);
    }
}

static void publish_telemetry_if_due(
    const sg_config_t *config,
    sg_mqtt_client_t *mqtt,
    const sg_vision_state_t *latest_state,
    double now_monotonic,
    double *last_telemetry_publish) {
    sg_vision_state_t telemetry_state;
    char payload[SG_PAYLOAD_SIZE];
    char error[SG_ERROR_SIZE];
    double temperature_c;
    time_t now;
    int result;

    if (!config->mqtt_enabled || mqtt == NULL ||
        (*last_telemetry_publish != 0.0 &&
         now_monotonic - *last_telemetry_publish <
             (double) config->mqtt_telemetry_interval_sec)) {
        return;
    }

    if (sg_read_cpu_temperature(
            config->cpu_temp_path,
            &temperature_c,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "WARNING MQTT telemetry skipped: %s\n", error);
        return;
    }

    now = time(NULL);
    if (now == (time_t) -1 ||
        sg_format_timestamp(
            now,
            telemetry_state.timestamp,
            sizeof(telemetry_state.timestamp)) != 0) {
        fprintf(stderr, "WARNING MQTT telemetry skipped: bad timestamp\n");
        return;
    }
    telemetry_state.persons = latest_state->persons;

    if (sg_build_event_payload(
            config,
            &telemetry_state,
            temperature_c,
            payload,
            sizeof(payload)) != 0) {
        fprintf(stderr, "ERROR Telemetry JSON payload is too long\n");
        return;
    }

    result = sg_mqtt_publish_telemetry(
        mqtt,
        payload,
        error,
        sizeof(error));
    if (result == 0) {
        *last_telemetry_publish = now_monotonic;
        fprintf(stderr, "INFO MQTT QoS 1 telemetry published: %s\n", payload);
    } else if (result < 0) {
        fprintf(stderr, "ERROR %s\n", error);
    }
}

int main(void) {
    sg_config_t config;
    sg_vision_state_t state = {0, ""};
    sg_mqtt_client_t *mqtt = NULL;
    struct sigaction action;
    char last_state_timestamp[SG_TIMESTAMP_SIZE] = "";
    char error[SG_ERROR_SIZE];
    time_t last_guard_alert_attempt = 0;
    double last_persons_publish = 0.0;
    double last_telemetry_publish = 0.0;
    double last_guard_warning = 0.0;
    int last_published_persons = -1;
    bool email_library_initialized = false;
    bool guard_enabled = false;
    bool previous_guard_enabled = false;
    bool guard_known = false;
    bool state_ready = false;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (sg_load_config(&config, error, sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Invalid notifier configuration: %s\n", error);
        return EXIT_FAILURE;
    }
    log_configuration(&config);

    if (sg_read_epoch_file(
            config.last_guard_alert_state_path,
            &last_guard_alert_attempt,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Cannot load guard alert state: %s\n", error);
        return EXIT_FAILURE;
    }

    if (config.email_enabled) {
        if (sg_email_global_init(error, sizeof(error)) != 0) {
            fprintf(stderr, "ERROR %s\n", error);
            return EXIT_FAILURE;
        }
        email_library_initialized = true;
    }

    if (config.mqtt_enabled &&
        sg_mqtt_start(&config, &mqtt, error, sizeof(error)) != 0) {
        fprintf(stderr, "ERROR MQTT startup failed: %s\n", error);
        if (email_library_initialized) {
            sg_email_global_cleanup();
        }
        return EXIT_FAILURE;
    }

    while (!stop_requested) {
        sg_vision_state_t candidate;
        double now_monotonic = monotonic_seconds();

        if (sg_guard_read(
                config.guard_state_path,
                config.guard_default_enabled,
                &guard_enabled,
                error,
                sizeof(error)) != 0) {
            guard_enabled = false;
            if (last_guard_warning == 0.0 ||
                now_monotonic - last_guard_warning >= 10.0) {
                fprintf(
                    stderr,
                    "ERROR Guard state is unreadable; failing closed: %s\n",
                    error);
                last_guard_warning = now_monotonic;
            }
        } else if (!guard_known || guard_enabled != previous_guard_enabled) {
            fprintf(
                stderr,
                "INFO Guard mode changed: %s\n",
                guard_enabled ? "ARMED" : "DISARMED");
            previous_guard_enabled = guard_enabled;
            guard_known = true;
        }

        if (sg_read_vision_state(
                config.vision_state_path,
                &candidate,
                error,
                sizeof(error)) == 0) {
            bool state_changed =
                strcmp(candidate.timestamp, last_state_timestamp) != 0 ||
                candidate.persons != state.persons;
            double temperature_c;

            state = candidate;
            state_ready = true;
            if (state_changed) {
                snprintf(
                    last_state_timestamp,
                    sizeof(last_state_timestamp),
                    "%s",
                    state.timestamp);
            }

            if (sg_read_cpu_temperature(
                    config.cpu_temp_path,
                    &temperature_c,
                    error,
                    sizeof(error)) == 0) {
                publish_persons_if_due(
                    &config,
                    mqtt,
                    &state,
                    temperature_c,
                    now_monotonic,
                    &last_published_persons,
                    &last_persons_publish);
                maybe_send_guard_alert(
                    &config,
                    mqtt,
                    guard_enabled,
                    &state,
                    temperature_c,
                    &last_guard_alert_attempt);
            } else if (state_changed) {
                fprintf(
                    stderr,
                    "WARNING Detection event has no CPU temperature: %s\n",
                    error);
            }
        } else if (!state_ready) {
            static double last_state_warning = 0.0;

            if (last_state_warning == 0.0 ||
                now_monotonic - last_state_warning >= 10.0) {
                fprintf(stderr, "WARNING Waiting for vision state: %s\n", error);
                last_state_warning = now_monotonic;
            }
        }

        if (state_ready) {
            publish_telemetry_if_due(
                &config,
                mqtt,
                &state,
                now_monotonic,
                &last_telemetry_publish);
        }
        sleep_milliseconds(config.poll_interval_ms);
    }

    fprintf(stderr, "INFO Smart Guard notifier is stopping\n");
    sg_mqtt_stop(mqtt);
    if (email_library_initialized) {
        sg_email_global_cleanup();
    }
    return EXIT_SUCCESS;
}
