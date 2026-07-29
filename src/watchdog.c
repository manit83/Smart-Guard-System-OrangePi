#define _POSIX_C_SOURCE 200809L

#include "email_sender.h"
#include "history_db.h"
#include "smart_guard_notifier.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number) {
    (void) signal_number;
    stop_requested = 1;
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

static unsigned long frame_age_seconds(
    const char *path,
    time_t now,
    time_t service_started) {
    struct stat information;
    time_t reference = service_started;

    if (stat(path, &information) == 0 && S_ISREG(information.st_mode)) {
        reference = information.st_mtime;
    }
    if (reference > now) {
        return 0;
    }
    return (unsigned long) difftime(now, reference);
}

static int restart_vision_service(const char *service) {
    const char *systemctl = access("/usr/bin/systemctl", X_OK) == 0
        ? "/usr/bin/systemctl"
        : "/bin/systemctl";
    pid_t child = fork();
    int status;

    if (child < 0) {
        fprintf(stderr, "ERROR Cannot fork for systemd restart: %s\n", strerror(errno));
        return -1;
    }
    if (child == 0) {
        execl(
            systemctl,
            "systemctl",
            "--no-block",
            "restart",
            service,
            (char *) NULL);
        _exit(127);
    }

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(
                stderr,
                "ERROR Cannot wait for systemd restart: %s\n",
                strerror(errno));
            return -1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(
            stderr,
            "ERROR systemctl restart %s failed with status %d\n",
            service,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    fprintf(stderr, "INFO Restart requested for %s\n", service);
    return 0;
}

static void record_detection_if_needed(
    const sg_config_t *config,
    sg_history_db_t *history,
    const sg_vision_state_t *state,
    int previous_persons,
    time_t now,
    time_t *last_history_event) {
    bool new_detection =
        state->persons > 0 &&
        (previous_persons <= 0 || state->persons > previous_persons);
    char error[SG_ERROR_SIZE];
    int result;
    int newly_detected_persons;

    if (!new_detection ||
        (*last_history_event > 0 &&
         difftime(now, *last_history_event) <
             (double) config->history_event_rearm_sec)) {
        return;
    }

    newly_detected_persons = previous_persons <= 0
        ? state->persons
        : state->persons - previous_persons;
    result = sg_history_record(
        history,
        state->timestamp,
        state->persons,
        newly_detected_persons,
        config->latest_frame_path,
        error,
        sizeof(error));
    if (result == 0) {
        *last_history_event = now;
        fprintf(
            stderr,
            "INFO Detection stored in SQLite: persons=%d timestamp=%s\n",
            state->persons,
            state->timestamp);
    } else if (result < 0) {
        fprintf(stderr, "ERROR Cannot store detection history: %s\n", error);
    }
}

static void handle_stale_frame(
    const sg_config_t *config,
    unsigned long age,
    time_t now,
    time_t *last_tamper_attempt) {
    char timestamp[SG_TIMESTAMP_SIZE];
    char error[SG_ERROR_SIZE];
    double temperature_c = -1.0;

    if (!sg_email_due(
            now,
            *last_tamper_attempt,
            config->watchdog_incident_debounce_sec)) {
        fprintf(
            stderr,
            "WARNING Camera frame is stale (%lus), but the persisted "
            "tamper debounce is active\n",
            age);
        return;
    }

    if (sg_atomic_write_epoch(
            config->last_tamper_state_path,
            now,
            error,
            sizeof(error)) != 0) {
        fprintf(
            stderr,
            "ERROR Tamper response blocked because state cannot be persisted: %s\n",
            error);
        return;
    }
    *last_tamper_attempt = now;

    if (sg_format_timestamp(now, timestamp, sizeof(timestamp)) != 0) {
        snprintf(timestamp, sizeof(timestamp), "unknown");
    }
    if (sg_read_cpu_temperature(
            config->cpu_temp_path,
            &temperature_c,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "WARNING Tamper email has no CPU temperature: %s\n", error);
    }

    fprintf(
        stderr,
        "WARNING CAMERA TAMPERING: no new frame for %lu seconds\n",
        age);

    if (config->watchdog_email_enabled) {
        if (sg_send_tamper_email(
                config,
                timestamp,
                temperature_c,
                age,
                error,
                sizeof(error)) != 0) {
            fprintf(stderr, "ERROR Camera tampering email failed: %s\n", error);
        } else {
            fprintf(stderr, "INFO Camera tampering email sent\n");
        }
    }

    if (config->watchdog_restart_enabled) {
        restart_vision_service(config->watchdog_restart_service);
    }
}

int main(int argc, char **argv) {
    sg_config_t config;
    sg_history_db_t *history = NULL;
    struct sigaction action;
    char last_state_timestamp[SG_TIMESTAMP_SIZE] = "";
    char error[SG_ERROR_SIZE];
    time_t service_started = time(NULL);
    time_t last_history_event = 0;
    time_t last_tamper_attempt = 0;
    int previous_persons = 0;
    bool incident_latched = false;
    bool email_library_initialized = false;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (sg_load_config(&config, error, sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Invalid watchdog configuration: %s\n", error);
        return EXIT_FAILURE;
    }

    if (sg_history_open(
            config.history_db_path,
            config.history_capacity,
            false,
            &history,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Cannot initialize SQLite history: %s\n", error);
        return EXIT_FAILURE;
    }

    if (argc == 2 && strcmp(argv[1], "--init-only") == 0) {
        fprintf(
            stderr,
            "INFO SQLite history initialized at %s with capacity %d\n",
            config.history_db_path,
            config.history_capacity);
        sg_history_close(history);
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: smart-guard-watchdog [--init-only]\n");
        sg_history_close(history);
        return EXIT_FAILURE;
    }

    if (sg_read_epoch_file(
            config.last_tamper_state_path,
            &last_tamper_attempt,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Cannot load tamper debounce state: %s\n", error);
        sg_history_close(history);
        return EXIT_FAILURE;
    }

    if (config.watchdog_email_enabled) {
        if (sg_email_global_init(error, sizeof(error)) != 0) {
            fprintf(stderr, "ERROR %s\n", error);
            sg_history_close(history);
            return EXIT_FAILURE;
        }
        email_library_initialized = true;
    }

    fprintf(
        stderr,
        "INFO Watchdog started: frame timeout=%ds, history=%s, capacity=%d\n",
        config.watchdog_frame_timeout_sec,
        config.history_db_path,
        config.history_capacity);

    while (!stop_requested) {
        sg_vision_state_t state;
        time_t now = time(NULL);
        unsigned long age;

        if (now == (time_t) -1) {
            sleep_milliseconds(config.watchdog_poll_interval_ms);
            continue;
        }

        if (sg_read_vision_state(
                config.vision_state_path,
                &state,
                error,
                sizeof(error)) == 0 &&
            strcmp(state.timestamp, last_state_timestamp) != 0) {
            record_detection_if_needed(
                &config,
                history,
                &state,
                previous_persons,
                now,
                &last_history_event);
            previous_persons = state.persons;
            snprintf(
                last_state_timestamp,
                sizeof(last_state_timestamp),
                "%s",
                state.timestamp);
        }

        age = frame_age_seconds(
            config.latest_frame_path,
            now,
            service_started);
        if (age >= (unsigned long) config.watchdog_frame_timeout_sec) {
            if (!incident_latched) {
                handle_stale_frame(
                    &config,
                    age,
                    now,
                    &last_tamper_attempt);
                incident_latched = true;
            }
        } else if (incident_latched) {
            fprintf(stderr, "INFO Camera frames recovered; watchdog re-armed\n");
            incident_latched = false;
        }

        sleep_milliseconds(config.watchdog_poll_interval_ms);
    }

    fprintf(stderr, "INFO Smart Guard watchdog is stopping\n");
    if (email_library_initialized) {
        sg_email_global_cleanup();
    }
    sg_history_close(history);
    return EXIT_SUCCESS;
}
