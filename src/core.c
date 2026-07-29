#define _POSIX_C_SOURCE 200809L

#include "smart_guard_notifier.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SG_VISION_JSON_MAX 8192
#define SG_THERMAL_ROOT "/sys/class/thermal"

static void set_error(char *error, size_t size, const char *format, ...) {
    va_list arguments;

    if (error == NULL || size == 0) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static int copy_text(
    char *destination,
    size_t destination_size,
    const char *source,
    const char *name,
    char *error,
    size_t error_size) {
    int length;

    if (source == NULL) {
        source = "";
    }

    length = snprintf(destination, destination_size, "%s", source);
    if (length < 0 || (size_t) length >= destination_size) {
        set_error(error, error_size, "%s is too long", name);
        return -1;
    }
    return 0;
}

static bool parse_bool_value(const char *value, bool default_value, bool *valid) {
    if (value == NULL || *value == '\0') {
        *valid = true;
        return default_value;
    }

    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0) {
        *valid = true;
        return true;
    }

    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0) {
        *valid = true;
        return false;
    }

    *valid = false;
    return false;
}

static int get_bool_env(
    const char *name,
    bool default_value,
    bool *output,
    char *error,
    size_t error_size) {
    bool valid;
    const char *value = getenv(name);

    *output = parse_bool_value(value, default_value, &valid);
    if (!valid) {
        set_error(error, error_size, "%s must be 0 or 1", name);
        return -1;
    }
    return 0;
}

static int get_int_env(
    const char *name,
    int default_value,
    int minimum,
    int maximum,
    int *output,
    char *error,
    size_t error_size) {
    const char *text = getenv(name);
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        *output = default_value;
        return 0;
    }

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        set_error(
            error,
            error_size,
            "%s must be an integer from %d through %d",
            name,
            minimum,
            maximum);
        return -1;
    }

    *output = (int) value;
    return 0;
}

static int get_text_env(
    const char *name,
    const char *default_value,
    char *output,
    size_t output_size,
    char *error,
    size_t error_size) {
    const char *value = getenv(name);
    return copy_text(
        output,
        output_size,
        value == NULL || *value == '\0' ? default_value : value,
        name,
        error,
        error_size);
}

static bool contains_header_break(const char *text) {
    return text != NULL &&
        (strchr(text, '\r') != NULL || strchr(text, '\n') != NULL);
}

static bool valid_student_id(const char *student_id) {
    const unsigned char *cursor = (const unsigned char *) student_id;

    if (student_id == NULL || *student_id == '\0' ||
        strcmp(student_id, "YOUR_STUDENT_ID") == 0) {
        return false;
    }

    while (*cursor != '\0') {
        if (!isalnum(*cursor) &&
            *cursor != '-' && *cursor != '_' && *cursor != '.') {
            return false;
        }
        cursor++;
    }
    return true;
}

static bool absolute_path_or_auto(const char *path) {
    return strcmp(path, "auto") == 0 || path[0] == '/';
}

int sg_load_config(sg_config_t *config, char *error, size_t error_size) {
    if (config == NULL) {
        set_error(error, error_size, "config is null");
        return -1;
    }

    memset(config, 0, sizeof(*config));

    if (get_text_env(
            "STUDENT_ID", "", config->student_id,
            sizeof(config->student_id), error, error_size) != 0 ||
        get_text_env(
            "VISION_STATE_PATH", "/run/smart-guard/vision-state.json",
            config->vision_state_path, sizeof(config->vision_state_path),
            error, error_size) != 0 ||
        get_text_env(
            "LATEST_FRAME_PATH", "/run/smart-guard/latest.jpg",
            config->latest_frame_path, sizeof(config->latest_frame_path),
            error, error_size) != 0 ||
        get_text_env(
            "CPU_TEMP_PATH", "auto", config->cpu_temp_path,
            sizeof(config->cpu_temp_path), error, error_size) != 0 ||
        get_text_env(
            "LAST_EMAIL_STATE_PATH",
            "/var/lib/smart-guard/last-email-attempt",
            config->last_email_state_path,
            sizeof(config->last_email_state_path),
            error,
            error_size) != 0 ||
        get_text_env(
            "GUARD_STATE_PATH",
            "/var/lib/smart-guard/guard-mode",
            config->guard_state_path,
            sizeof(config->guard_state_path),
            error,
            error_size) != 0 ||
        get_text_env(
            "LAST_GUARD_ALERT_STATE_PATH",
            "/var/lib/smart-guard/last-guard-alert-attempt",
            config->last_guard_alert_state_path,
            sizeof(config->last_guard_alert_state_path),
            error,
            error_size) != 0 ||
        get_text_env(
            "HISTORY_DB_PATH",
            "/var/lib/smart-guard/history.db",
            config->history_db_path,
            sizeof(config->history_db_path),
            error,
            error_size) != 0 ||
        get_text_env(
            "LAST_TAMPER_STATE_PATH",
            "/var/lib/smart-guard/last-tamper-attempt",
            config->last_tamper_state_path,
            sizeof(config->last_tamper_state_path),
            error,
            error_size) != 0 ||
        get_int_env(
            "NOTIFIER_POLL_INTERVAL_MS", 250, 100, 5000,
            &config->poll_interval_ms, error, error_size) != 0 ||
        get_bool_env(
            "GUARD_DEFAULT_ENABLED", false,
            &config->guard_default_enabled, error, error_size) != 0 ||
        get_int_env(
            "GUARD_ALERT_DEBOUNCE_SEC", 30, 30, 86400,
            &config->guard_alert_debounce_sec, error, error_size) != 0 ||
        get_int_env(
            "HISTORY_CAPACITY", 1000, 5, 100000,
            &config->history_capacity, error, error_size) != 0 ||
        get_int_env(
            "HISTORY_EVENT_REARM_SEC", 2, 1, 3600,
            &config->history_event_rearm_sec, error, error_size) != 0) {
        return -1;
    }

    if (!valid_student_id(config->student_id)) {
        set_error(
            error,
            error_size,
            "STUDENT_ID must use only letters, digits, dot, underscore, or dash");
        return -1;
    }

    if (config->vision_state_path[0] != '/' ||
        config->latest_frame_path[0] != '/' ||
        config->last_email_state_path[0] != '/' ||
        config->guard_state_path[0] != '/' ||
        config->last_guard_alert_state_path[0] != '/' ||
        config->history_db_path[0] != '/' ||
        config->last_tamper_state_path[0] != '/' ||
        !absolute_path_or_auto(config->cpu_temp_path)) {
        set_error(error, error_size, "all configured file paths must be absolute");
        return -1;
    }

    if (get_bool_env(
            "EMAIL_ENABLED", false, &config->email_enabled,
            error, error_size) != 0 ||
        get_text_env(
            "EMAIL_TO", "", config->email_to, sizeof(config->email_to),
            error, error_size) != 0 ||
        get_text_env(
            "SMTP_HOST", "", config->smtp_host, sizeof(config->smtp_host),
            error, error_size) != 0 ||
        get_int_env(
            "SMTP_PORT", 587, 1, 65535, &config->smtp_port,
            error, error_size) != 0 ||
        get_text_env(
            "SMTP_SECURITY", "starttls", config->smtp_security,
            sizeof(config->smtp_security), error, error_size) != 0 ||
        get_text_env(
            "SMTP_USERNAME", "", config->smtp_username,
            sizeof(config->smtp_username), error, error_size) != 0 ||
        get_text_env(
            "SMTP_FROM", "", config->smtp_from, sizeof(config->smtp_from),
            error, error_size) != 0 ||
        get_text_env(
            "SMTP_PASSWORD_FILE", "/etc/smart-guard/smtp-password",
            config->smtp_password_file, sizeof(config->smtp_password_file),
            error, error_size) != 0 ||
        get_text_env(
            "SMTP_CA_FILE", "", config->smtp_ca_file,
            sizeof(config->smtp_ca_file), error, error_size) != 0 ||
        get_text_env(
            "EMAIL_SUBJECT_PREFIX", "Smart Guard",
            config->email_subject_prefix,
            sizeof(config->email_subject_prefix),
            error,
            error_size) != 0 ||
        get_int_env(
            "EMAIL_DEBOUNCE_SEC", 30, 30, 86400,
            &config->email_debounce_sec, error, error_size) != 0) {
        return -1;
    }

    if (get_bool_env(
            "WATCHDOG_EMAIL_ENABLED", false,
            &config->watchdog_email_enabled, error, error_size) != 0 ||
        get_bool_env(
            "WATCHDOG_RESTART_ENABLED", true,
            &config->watchdog_restart_enabled, error, error_size) != 0 ||
        get_int_env(
            "WATCHDOG_FRAME_TIMEOUT_SEC", 30, 30, 3600,
            &config->watchdog_frame_timeout_sec, error, error_size) != 0 ||
        get_int_env(
            "WATCHDOG_POLL_INTERVAL_MS", 500, 100, 5000,
            &config->watchdog_poll_interval_ms, error, error_size) != 0 ||
        get_int_env(
            "WATCHDOG_INCIDENT_DEBOUNCE_SEC", 300, 30, 86400,
            &config->watchdog_incident_debounce_sec, error, error_size) != 0 ||
        get_text_env(
            "WATCHDOG_RESTART_SERVICE",
            "smart-guard-vision.service",
            config->watchdog_restart_service,
            sizeof(config->watchdog_restart_service),
            error,
            error_size) != 0) {
        return -1;
    }

    if (config->email_enabled || config->watchdog_email_enabled) {
        if (config->email_to[0] == '\0' ||
            config->smtp_host[0] == '\0' ||
            config->smtp_username[0] == '\0' ||
            config->smtp_from[0] == '\0') {
            set_error(
                error,
                error_size,
                "EMAIL_TO, SMTP_HOST, SMTP_USERNAME, and SMTP_FROM are required");
            return -1;
        }
        if (strcmp(config->smtp_security, "starttls") != 0 &&
            strcmp(config->smtp_security, "tls") != 0 &&
            strcmp(config->smtp_security, "none") != 0) {
            set_error(
                error,
                error_size,
                "SMTP_SECURITY must be starttls, tls, or none");
            return -1;
        }
        if (config->smtp_password_file[0] != '/' ||
            (config->smtp_ca_file[0] != '\0' &&
             config->smtp_ca_file[0] != '/')) {
            set_error(error, error_size, "SMTP password and CA paths must be absolute");
            return -1;
        }
        if (contains_header_break(config->email_to) ||
            contains_header_break(config->smtp_from) ||
            contains_header_break(config->email_subject_prefix)) {
            set_error(error, error_size, "email headers contain a line break");
            return -1;
        }
    }

    if (config->watchdog_restart_service[0] == '\0' ||
        strchr(config->watchdog_restart_service, '/') != NULL ||
        contains_header_break(config->watchdog_restart_service)) {
        set_error(
            error,
            error_size,
            "WATCHDOG_RESTART_SERVICE must be a systemd unit name");
        return -1;
    }

    if (get_bool_env(
            "MQTT_ENABLED", false, &config->mqtt_enabled,
            error, error_size) != 0 ||
        get_text_env(
            "MQTT_BROKER_HOST", "", config->mqtt_broker_host,
            sizeof(config->mqtt_broker_host), error, error_size) != 0 ||
        get_int_env(
            "MQTT_BROKER_PORT", 1883, 1, 65535,
            &config->mqtt_broker_port, error, error_size) != 0 ||
        get_text_env(
            "MQTT_USERNAME", "", config->mqtt_username,
            sizeof(config->mqtt_username), error, error_size) != 0 ||
        get_text_env(
            "MQTT_PASSWORD_FILE", "/etc/smart-guard/mqtt-password",
            config->mqtt_password_file, sizeof(config->mqtt_password_file),
            error, error_size) != 0 ||
        get_int_env(
            "MQTT_KEEPALIVE_SEC", 15, 5, 3600,
            &config->mqtt_keepalive_sec, error, error_size) != 0 ||
        get_int_env(
            "MQTT_TELEMETRY_INTERVAL_SEC", 5, 1, 3600,
            &config->mqtt_telemetry_interval_sec, error, error_size) != 0 ||
        get_int_env(
            "MQTT_PERSONS_INTERVAL_SEC", 1, 1, 3600,
            &config->mqtt_persons_interval_sec, error, error_size) != 0) {
        return -1;
    }

    if (config->mqtt_enabled) {
        if (config->mqtt_broker_host[0] == '\0' ||
            config->mqtt_username[0] == '\0') {
            set_error(
                error,
                error_size,
                "MQTT_BROKER_HOST and MQTT_USERNAME are required");
            return -1;
        }
        if (config->mqtt_password_file[0] != '/') {
            set_error(error, error_size, "MQTT_PASSWORD_FILE must be absolute");
            return -1;
        }
    }

    return 0;
}

static const char *find_json_value(const char *json, const char *key) {
    char pattern[96];
    const char *cursor;
    int length;

    length = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (length < 0 || (size_t) length >= sizeof(pattern)) {
        return NULL;
    }

    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return NULL;
    }
    cursor += strlen(pattern);

    while (isspace((unsigned char) *cursor)) {
        cursor++;
    }
    if (*cursor != ':') {
        return NULL;
    }
    cursor++;
    while (isspace((unsigned char) *cursor)) {
        cursor++;
    }
    return cursor;
}

int sg_parse_vision_state_json(
    const char *json,
    sg_vision_state_t *state,
    char *error,
    size_t error_size) {
    const char *persons_value;
    const char *timestamp_value;
    char *persons_end = NULL;
    long persons;
    size_t timestamp_length = 0;

    if (json == NULL || state == NULL) {
        set_error(error, error_size, "vision state input is null");
        return -1;
    }

    persons_value = find_json_value(json, "persons");
    timestamp_value = find_json_value(json, "timestamp");
    if (persons_value == NULL || timestamp_value == NULL) {
        set_error(error, error_size, "vision JSON needs persons and timestamp");
        return -1;
    }

    errno = 0;
    persons = strtol(persons_value, &persons_end, 10);
    if (errno != 0 || persons_end == persons_value ||
        persons < 0 || persons > 10000) {
        set_error(error, error_size, "vision JSON has an invalid persons value");
        return -1;
    }

    if (*timestamp_value != '"') {
        set_error(error, error_size, "vision JSON timestamp must be a string");
        return -1;
    }
    timestamp_value++;

    while (timestamp_value[timestamp_length] != '\0' &&
           timestamp_value[timestamp_length] != '"') {
        unsigned char character =
            (unsigned char) timestamp_value[timestamp_length];
        if (character < 0x20 || character == '\\') {
            set_error(
                error,
                error_size,
                "vision JSON timestamp uses unsupported escaping");
            return -1;
        }
        timestamp_length++;
    }

    if (timestamp_value[timestamp_length] != '"' ||
        timestamp_length == 0 ||
        timestamp_length >= sizeof(state->timestamp)) {
        set_error(error, error_size, "vision JSON timestamp is invalid or too long");
        return -1;
    }

    state->persons = (int) persons;
    memcpy(state->timestamp, timestamp_value, timestamp_length);
    state->timestamp[timestamp_length] = '\0';
    return 0;
}

int sg_read_vision_state(
    const char *path,
    sg_vision_state_t *state,
    char *error,
    size_t error_size) {
    FILE *file;
    char buffer[SG_VISION_JSON_MAX + 1];
    size_t bytes_read;

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    bytes_read = fread(buffer, 1, SG_VISION_JSON_MAX, file);
    if (ferror(file)) {
        set_error(error, error_size, "cannot read %s: %s", path, strerror(errno));
        fclose(file);
        return -1;
    }
    if (!feof(file)) {
        set_error(error, error_size, "vision state exceeds %d bytes", SG_VISION_JSON_MAX);
        fclose(file);
        return -1;
    }
    fclose(file);

    buffer[bytes_read] = '\0';
    return sg_parse_vision_state_json(buffer, state, error, error_size);
}

static int read_first_line(const char *path, char *buffer, size_t buffer_size) {
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return -1;
    }
    if (fgets(buffer, (int) buffer_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

static void lowercase(char *text) {
    unsigned char *cursor = (unsigned char *) text;

    while (*cursor != '\0') {
        *cursor = (unsigned char) tolower(*cursor);
        cursor++;
    }
}

static int read_temperature_value(const char *path, double *temperature_c) {
    char buffer[64];
    char *end = NULL;
    double value;

    if (read_first_line(path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    errno = 0;
    value = strtod(buffer, &end);
    while (end != NULL && isspace((unsigned char) *end)) {
        end++;
    }
    if (errno != 0 || end == buffer || (end != NULL && *end != '\0')) {
        return -1;
    }

    if (value > 1000.0) {
        value /= 1000.0;
    }
    if (value < -40.0 || value > 180.0) {
        return -1;
    }

    *temperature_c = value;
    return 0;
}

int sg_read_cpu_temperature(
    const char *configured_path,
    double *temperature_c,
    char *error,
    size_t error_size) {
    DIR *directory;
    struct dirent *entry;
    char fallback_path[SG_PATH_SIZE] = "";

    if (configured_path == NULL || temperature_c == NULL) {
        set_error(error, error_size, "temperature input is null");
        return -1;
    }

    if (strcmp(configured_path, "auto") != 0) {
        if (read_temperature_value(configured_path, temperature_c) != 0) {
            set_error(
                error,
                error_size,
                "cannot read CPU temperature from %s",
                configured_path);
            return -1;
        }
        return 0;
    }

    directory = opendir(SG_THERMAL_ROOT);
    if (directory != NULL) {
        while ((entry = readdir(directory)) != NULL) {
            char type_path[SG_PATH_SIZE];
            char temp_path[SG_PATH_SIZE];
            char type[128];
            int type_length;
            int temp_length;

            if (strncmp(entry->d_name, "thermal_zone", 12) != 0) {
                continue;
            }

            type_length = snprintf(
                type_path,
                sizeof(type_path),
                "%s/%s/type",
                SG_THERMAL_ROOT,
                entry->d_name);
            temp_length = snprintf(
                temp_path,
                sizeof(temp_path),
                "%s/%s/temp",
                SG_THERMAL_ROOT,
                entry->d_name);
            if (type_length < 0 || (size_t) type_length >= sizeof(type_path) ||
                temp_length < 0 || (size_t) temp_length >= sizeof(temp_path)) {
                continue;
            }

            if (fallback_path[0] == '\0') {
                snprintf(fallback_path, sizeof(fallback_path), "%s", temp_path);
            }

            if (read_first_line(type_path, type, sizeof(type)) != 0) {
                continue;
            }
            lowercase(type);
            if (strstr(type, "cpu") != NULL ||
                strstr(type, "soc") != NULL ||
                strstr(type, "package") != NULL) {
                if (read_temperature_value(temp_path, temperature_c) == 0) {
                    closedir(directory);
                    return 0;
                }
            }
        }
        closedir(directory);
    }

    if (fallback_path[0] != '\0' &&
        read_temperature_value(fallback_path, temperature_c) == 0) {
        return 0;
    }

    if (read_temperature_value(
            "/sys/devices/virtual/thermal/thermal_zone0/temp",
            temperature_c) == 0) {
        return 0;
    }

    set_error(
        error,
        error_size,
        "no readable CPU thermal zone was found under %s",
        SG_THERMAL_ROOT);
    return -1;
}

int sg_build_event_payload(
    const sg_config_t *config,
    const sg_vision_state_t *state,
    double temperature_c,
    char *payload,
    size_t payload_size) {
    int length;

    if (config == NULL || state == NULL || payload == NULL) {
        return -1;
    }

    length = snprintf(
        payload,
        payload_size,
        "{\"student_id\":\"%s\",\"persons\":%d,"
        "\"temperature_c\":%.1f,\"timestamp\":\"%s\"}",
        config->student_id,
        state->persons,
        temperature_c,
        state->timestamp);
    if (length < 0 || (size_t) length >= payload_size) {
        return -1;
    }
    return 0;
}

int sg_format_timestamp(
    time_t timestamp,
    char *buffer,
    size_t buffer_size) {
    struct tm local_time;
    char date[32];
    char zone[16];
    int length;

    if (buffer == NULL || buffer_size < 26 ||
        localtime_r(&timestamp, &local_time) == NULL) {
        return -1;
    }
    if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local_time) == 0 ||
        strftime(zone, sizeof(zone), "%z", &local_time) == 0) {
        return -1;
    }

    if (strlen(zone) == 5) {
        length = snprintf(
            buffer,
            buffer_size,
            "%s%c%c%c:%c%c",
            date,
            zone[0],
            zone[1],
            zone[2],
            zone[3],
            zone[4]);
    } else {
        length = snprintf(buffer, buffer_size, "%s%s", date, zone);
    }

    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

bool sg_email_due(time_t now, time_t last_attempt, int debounce_sec) {
    if (last_attempt <= 0) {
        return true;
    }
    if (now <= last_attempt) {
        return false;
    }
    return difftime(now, last_attempt) >= (double) debounce_sec;
}

int sg_read_epoch_file(
    const char *path,
    time_t *value,
    char *error,
    size_t error_size) {
    FILE *file;
    char buffer[64];
    char *end = NULL;
    intmax_t parsed;

    if (value == NULL) {
        set_error(error, error_size, "epoch output is null");
        return -1;
    }
    *value = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        set_error(error, error_size, "cannot read %s", path);
        fclose(file);
        return -1;
    }
    fclose(file);

    errno = 0;
    parsed = strtoimax(buffer, &end, 10);
    while (end != NULL && isspace((unsigned char) *end)) {
        end++;
    }
    if (errno != 0 || end == buffer || (end != NULL && *end != '\0') ||
        parsed < 0) {
        set_error(error, error_size, "%s contains an invalid epoch", path);
        return -1;
    }

    *value = (time_t) parsed;
    return 0;
}

static int write_all(int descriptor, const char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(descriptor, buffer + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t) written;
    }
    return 0;
}

int sg_atomic_write_epoch(
    const char *path,
    time_t value,
    char *error,
    size_t error_size) {
    char temporary_path[SG_PATH_SIZE + 64];
    char data[64];
    int temporary_length;
    int data_length;
    int descriptor;
    int saved_errno;

    temporary_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp.%ld",
        path,
        (long) getpid());
    data_length = snprintf(data, sizeof(data), "%jd\n", (intmax_t) value);
    if (temporary_length < 0 ||
        (size_t) temporary_length >= sizeof(temporary_path) ||
        data_length < 0 || (size_t) data_length >= sizeof(data)) {
        set_error(error, error_size, "debounce state path is too long");
        return -1;
    }

    descriptor = open(
        temporary_path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP);
    if (descriptor < 0) {
        set_error(
            error,
            error_size,
            "cannot create %s: %s",
            temporary_path,
            strerror(errno));
        return -1;
    }

    if (write_all(descriptor, data, (size_t) data_length) != 0 ||
        fsync(descriptor) != 0) {
        saved_errno = errno;
        close(descriptor);
        unlink(temporary_path);
        set_error(error, error_size, "cannot persist debounce state: %s", strerror(saved_errno));
        return -1;
    }
    if (close(descriptor) != 0) {
        saved_errno = errno;
        unlink(temporary_path);
        set_error(error, error_size, "cannot close debounce state: %s", strerror(saved_errno));
        return -1;
    }

    if (rename(temporary_path, path) != 0) {
        saved_errno = errno;
        unlink(temporary_path);
        set_error(error, error_size, "cannot replace %s: %s", path, strerror(saved_errno));
        return -1;
    }
    return 0;
}

int sg_read_secret(
    const char *path,
    char *buffer,
    size_t buffer_size,
    char *error,
    size_t error_size) {
    FILE *file;
    size_t length;

    if (path == NULL || buffer == NULL || buffer_size < 2) {
        set_error(error, error_size, "secret input is invalid");
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        set_error(error, error_size, "cannot open secret file %s: %s", path, strerror(errno));
        return -1;
    }

    if (fgets(buffer, (int) buffer_size, file) == NULL) {
        set_error(error, error_size, "cannot read secret file %s", path);
        fclose(file);
        return -1;
    }
    if (!feof(file)) {
        int character = fgetc(file);
        if (character != EOF) {
            set_error(error, error_size, "secret in %s is too long", path);
            fclose(file);
            return -1;
        }
    }
    fclose(file);

    length = strcspn(buffer, "\r\n");
    buffer[length] = '\0';
    if (length == 0) {
        set_error(error, error_size, "secret file %s is empty", path);
        return -1;
    }
    return 0;
}
