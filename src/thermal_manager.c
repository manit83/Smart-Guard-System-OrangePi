#define _POSIX_C_SOURCE 200809L

#include "email_sender.h"
#include "smart_guard_notifier.h"
#include "thermal_policy.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    sg_config_t shared;
    bool enabled;
    bool email_enabled;
    bool restart_enabled;
    double high_temperature_c;
    double recovery_temperature_c;
    int poll_interval_sec;
    int email_retry_sec;

    int normal_width;
    int normal_height;
    int normal_input_fps;
    double normal_output_fps;
    int normal_detection_width;

    int hot_width;
    int hot_height;
    int hot_input_fps;
    double hot_output_fps;
    int hot_detection_width;

    char override_path[SG_PATH_SIZE];
    char status_path[SG_PATH_SIZE];
    char state_path[SG_PATH_SIZE];
    char last_email_mode_path[SG_PATH_SIZE];
    char last_email_attempt_path[SG_PATH_SIZE];
    char restart_service[SG_TEXT_SIZE];
} thermal_config_t;

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number) {
    (void) signal_number;
    stop_requested = 1;
}

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
    int length = snprintf(
        destination,
        destination_size,
        "%s",
        source == NULL ? "" : source);

    if (length < 0 || (size_t) length >= destination_size) {
        set_error(error, error_size, "%s is too long", name);
        return -1;
    }
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

static int get_bool_env(
    const char *name,
    bool default_value,
    bool *output,
    char *error,
    size_t error_size) {
    const char *value = getenv(name);

    if (value == NULL || *value == '\0') {
        *output = default_value;
        return 0;
    }
    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0) {
        *output = true;
        return 0;
    }
    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0) {
        *output = false;
        return 0;
    }

    set_error(error, error_size, "%s must be 0 or 1", name);
    return -1;
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

static int get_double_env(
    const char *name,
    double default_value,
    double minimum,
    double maximum,
    double *output,
    char *error,
    size_t error_size) {
    const char *text = getenv(name);
    char *end = NULL;
    double value;

    if (text == NULL || *text == '\0') {
        *output = default_value;
        return 0;
    }

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        set_error(
            error,
            error_size,
            "%s must be a number from %.1f through %.1f",
            name,
            minimum,
            maximum);
        return -1;
    }
    *output = value;
    return 0;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    size_t prefix_length = strlen(prefix);

    return strncmp(path, prefix, prefix_length) == 0 &&
        path[prefix_length] != '\0';
}

static bool valid_unit_name(const char *name) {
    const unsigned char *cursor = (const unsigned char *) name;

    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL) {
        return false;
    }
    while (*cursor != '\0') {
        if (!isalnum(*cursor) &&
            *cursor != '-' && *cursor != '_' &&
            *cursor != '.' && *cursor != '@') {
            return false;
        }
        cursor++;
    }
    return true;
}

static int minimum_int(int first, int second) {
    return first < second ? first : second;
}

static double minimum_double(double first, double second) {
    return first < second ? first : second;
}

static int validate_email_config(
    const thermal_config_t *config,
    char *error,
    size_t error_size) {
    if (!config->email_enabled) {
        return 0;
    }

    if (config->shared.email_to[0] == '\0' ||
        config->shared.smtp_host[0] == '\0' ||
        config->shared.smtp_username[0] == '\0' ||
        config->shared.smtp_from[0] == '\0') {
        set_error(
            error,
            error_size,
            "thermal email needs EMAIL_TO, SMTP_HOST, "
            "SMTP_USERNAME, and SMTP_FROM");
        return -1;
    }
    if (strcmp(config->shared.smtp_security, "starttls") != 0 &&
        strcmp(config->shared.smtp_security, "tls") != 0 &&
        strcmp(config->shared.smtp_security, "none") != 0) {
        set_error(
            error,
            error_size,
            "SMTP_SECURITY must be starttls, tls, or none");
        return -1;
    }
    if (config->shared.smtp_password_file[0] != '/') {
        set_error(
            error,
            error_size,
            "SMTP_PASSWORD_FILE must be an absolute path");
        return -1;
    }
    return 0;
}

static int load_thermal_config(
    thermal_config_t *config,
    char *error,
    size_t error_size) {
    int requested_hot_width;
    int requested_hot_height;
    int requested_hot_input_fps;
    double requested_hot_output_fps;
    int requested_hot_detection_width;

    memset(config, 0, sizeof(*config));
    if (sg_load_config(&config->shared, error, error_size) != 0) {
        return -1;
    }

    if (get_bool_env(
            "THERMAL_MANAGER_ENABLED", true,
            &config->enabled, error, error_size) != 0 ||
        get_bool_env(
            "THERMAL_EMAIL_ENABLED", true,
            &config->email_enabled, error, error_size) != 0 ||
        get_bool_env(
            "THERMAL_RESTART_ENABLED", true,
            &config->restart_enabled, error, error_size) != 0 ||
        get_double_env(
            "THERMAL_HIGH_TEMP_C", 70.0, 40.0, 110.0,
            &config->high_temperature_c, error, error_size) != 0 ||
        get_double_env(
            "THERMAL_RECOVERY_TEMP_C", 60.0, 30.0, 105.0,
            &config->recovery_temperature_c, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_POLL_INTERVAL_SEC", 2, 1, 60,
            &config->poll_interval_sec, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_EMAIL_RETRY_SEC", 60, 30, 3600,
            &config->email_retry_sec, error, error_size) != 0 ||
        get_int_env(
            "CAMERA_WIDTH", 640, 160, 3840,
            &config->normal_width, error, error_size) != 0 ||
        get_int_env(
            "CAMERA_HEIGHT", 480, 120, 2160,
            &config->normal_height, error, error_size) != 0 ||
        get_int_env(
            "CAMERA_INPUT_FPS", 10, 1, 60,
            &config->normal_input_fps, error, error_size) != 0 ||
        get_double_env(
            "VISION_OUTPUT_FPS", 2.0, 0.2, 30.0,
            &config->normal_output_fps, error, error_size) != 0 ||
        get_int_env(
            "VISION_DETECTION_WIDTH", 320, 160, 1280,
            &config->normal_detection_width, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_HOT_CAMERA_WIDTH", 320, 160, 3840,
            &requested_hot_width, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_HOT_CAMERA_HEIGHT", 240, 120, 2160,
            &requested_hot_height, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_HOT_CAMERA_INPUT_FPS", 5, 1, 60,
            &requested_hot_input_fps, error, error_size) != 0 ||
        get_double_env(
            "THERMAL_HOT_VISION_OUTPUT_FPS", 1.0, 0.2, 30.0,
            &requested_hot_output_fps, error, error_size) != 0 ||
        get_int_env(
            "THERMAL_HOT_DETECTION_WIDTH", 240, 160, 1280,
            &requested_hot_detection_width, error, error_size) != 0 ||
        get_text_env(
            "THERMAL_OVERRIDE_PATH",
            "/run/smart-guard/vision-thermal.env",
            config->override_path, sizeof(config->override_path),
            error, error_size) != 0 ||
        get_text_env(
            "THERMAL_STATUS_PATH",
            "/run/smart-guard/thermal-status.json",
            config->status_path, sizeof(config->status_path),
            error, error_size) != 0 ||
        get_text_env(
            "THERMAL_STATE_PATH",
            "/var/lib/smart-guard/thermal-mode",
            config->state_path, sizeof(config->state_path),
            error, error_size) != 0 ||
        get_text_env(
            "THERMAL_LAST_EMAIL_MODE_PATH",
            "/var/lib/smart-guard/thermal-last-email-mode",
            config->last_email_mode_path,
            sizeof(config->last_email_mode_path),
            error, error_size) != 0 ||
        get_text_env(
            "THERMAL_LAST_EMAIL_ATTEMPT_PATH",
            "/var/lib/smart-guard/thermal-last-email-attempt",
            config->last_email_attempt_path,
            sizeof(config->last_email_attempt_path),
            error, error_size) != 0 ||
        get_text_env(
            "THERMAL_RESTART_SERVICE",
            "smart-guard-vision.service",
            config->restart_service, sizeof(config->restart_service),
            error, error_size) != 0) {
        return -1;
    }

    if (config->recovery_temperature_c >= config->high_temperature_c) {
        set_error(
            error,
            error_size,
            "THERMAL_RECOVERY_TEMP_C must be lower than THERMAL_HIGH_TEMP_C");
        return -1;
    }
    if (!path_has_prefix(config->override_path, "/run/smart-guard/") ||
        !path_has_prefix(config->status_path, "/run/smart-guard/") ||
        !path_has_prefix(config->state_path, "/var/lib/smart-guard/") ||
        !path_has_prefix(
            config->last_email_mode_path,
            "/var/lib/smart-guard/") ||
        !path_has_prefix(
            config->last_email_attempt_path,
            "/var/lib/smart-guard/")) {
        set_error(
            error,
            error_size,
            "thermal runtime paths must be under /run/smart-guard "
            "and persistent paths under /var/lib/smart-guard");
        return -1;
    }
    if (!valid_unit_name(config->restart_service)) {
        set_error(
            error,
            error_size,
            "THERMAL_RESTART_SERVICE must be a systemd unit name");
        return -1;
    }

    config->hot_width = minimum_int(
        config->normal_width,
        requested_hot_width);
    config->hot_height = minimum_int(
        config->normal_height,
        requested_hot_height);
    config->hot_input_fps = minimum_int(
        config->normal_input_fps,
        requested_hot_input_fps);
    config->hot_output_fps = minimum_double(
        config->normal_output_fps,
        requested_hot_output_fps);
    config->hot_detection_width = minimum_int(
        config->normal_detection_width,
        requested_hot_detection_width);

    return validate_email_config(config, error, error_size);
}

static int write_all(int descriptor, const char *data, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        ssize_t written = write(descriptor, data + offset, size - offset);
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

static int read_small_file(
    const char *path,
    char *buffer,
    size_t buffer_size) {
    FILE *file;
    size_t bytes_read;

    if (buffer_size == 0) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? 1 : -1;
    }
    bytes_read = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buffer[bytes_read] = '\0';
    return 0;
}

static int atomic_write_if_changed(
    const char *path,
    const char *data,
    mode_t mode,
    bool *changed,
    char *error,
    size_t error_size) {
    char existing[2048];
    char temporary[SG_PATH_SIZE + 32];
    size_t data_size = strlen(data);
    int read_result;
    int descriptor;
    int saved_errno;
    int length;

    *changed = false;
    read_result = read_small_file(path, existing, sizeof(existing));
    if (read_result == 0 && strcmp(existing, data) == 0) {
        return 0;
    }
    if (read_result < 0) {
        set_error(error, error_size, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }

    length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t) length >= sizeof(temporary)) {
        set_error(error, error_size, "temporary path is too long");
        return -1;
    }
    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        set_error(
            error,
            error_size,
            "cannot create temporary file for %s: %s",
            path,
            strerror(errno));
        return -1;
    }

    if (fchmod(descriptor, mode) != 0 ||
        write_all(descriptor, data, data_size) != 0 ||
        fsync(descriptor) != 0) {
        saved_errno = errno;
        close(descriptor);
        unlink(temporary);
        set_error(error, error_size, "cannot write %s: %s", path, strerror(saved_errno));
        return -1;
    }
    if (close(descriptor) != 0) {
        saved_errno = errno;
        unlink(temporary);
        set_error(error, error_size, "cannot close %s: %s", path, strerror(saved_errno));
        return -1;
    }
    if (rename(temporary, path) != 0) {
        saved_errno = errno;
        unlink(temporary);
        set_error(error, error_size, "cannot replace %s: %s", path, strerror(saved_errno));
        return -1;
    }

    *changed = true;
    return 0;
}

static int remove_if_present(
    const char *path,
    bool *changed,
    char *error,
    size_t error_size) {
    *changed = false;
    if (unlink(path) == 0) {
        *changed = true;
        return 0;
    }
    if (errno == ENOENT) {
        return 0;
    }
    set_error(error, error_size, "cannot remove %s: %s", path, strerror(errno));
    return -1;
}

static int read_mode_file(
    const char *path,
    sg_thermal_mode_t *mode,
    bool *present) {
    char buffer[64];
    int result = read_small_file(path, buffer, sizeof(buffer));

    *present = false;
    *mode = SG_THERMAL_NORMAL;
    if (result == 1) {
        return 0;
    }
    if (result != 0) {
        return -1;
    }

    buffer[strcspn(buffer, "\r\n")] = '\0';
    if (strcmp(buffer, "HOT") == 0) {
        *mode = SG_THERMAL_HOT;
        *present = true;
    } else if (strcmp(buffer, "NORMAL") == 0) {
        *mode = SG_THERMAL_NORMAL;
        *present = true;
    }
    return 0;
}

static int persist_mode(
    const char *path,
    sg_thermal_mode_t mode,
    bool *changed,
    char *error,
    size_t error_size) {
    char data[32];

    snprintf(data, sizeof(data), "%s\n", sg_thermal_mode_name(mode));
    return atomic_write_if_changed(
        path,
        data,
        S_IRUSR | S_IWUSR | S_IRGRP,
        changed,
        error,
        error_size);
}

static int build_hot_override(
    const thermal_config_t *config,
    char *buffer,
    size_t buffer_size) {
    int length = snprintf(
        buffer,
        buffer_size,
        "# Managed by smart-guard-thermal-manager\n"
        "CAMERA_WIDTH=%d\n"
        "CAMERA_HEIGHT=%d\n"
        "CAMERA_INPUT_FPS=%d\n"
        "VISION_OUTPUT_FPS=%.2f\n"
        "VISION_DETECTION_WIDTH=%d\n",
        config->hot_width,
        config->hot_height,
        config->hot_input_fps,
        config->hot_output_fps,
        config->hot_detection_width);

    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

static int synchronize_profile(
    const thermal_config_t *config,
    sg_thermal_mode_t mode,
    bool *override_changed,
    char *error,
    size_t error_size) {
    char override_data[512];

    if (mode == SG_THERMAL_NORMAL) {
        return remove_if_present(
            config->override_path,
            override_changed,
            error,
            error_size);
    }

    if (build_hot_override(
            config,
            override_data,
            sizeof(override_data)) != 0) {
        set_error(error, error_size, "thermal override data is too long");
        return -1;
    }
    return atomic_write_if_changed(
        config->override_path,
        override_data,
        S_IRUSR | S_IWUSR | S_IRGRP,
        override_changed,
        error,
        error_size);
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
            fprintf(stderr, "ERROR Cannot wait for systemctl: %s\n", strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(
            stderr,
            "ERROR Restart of %s failed with status %d\n",
            service,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }

    fprintf(stderr, "INFO Restart requested for %s\n", service);
    return 0;
}

static int write_status(
    const thermal_config_t *config,
    sg_thermal_mode_t mode,
    double temperature_c,
    char *error,
    size_t error_size) {
    char timestamp[SG_TIMESTAMP_SIZE];
    char data[1024];
    bool changed;
    time_t now = time(NULL);
    int active_width = mode == SG_THERMAL_HOT
        ? config->hot_width : config->normal_width;
    int active_height = mode == SG_THERMAL_HOT
        ? config->hot_height : config->normal_height;
    int active_input_fps = mode == SG_THERMAL_HOT
        ? config->hot_input_fps : config->normal_input_fps;
    double active_output_fps = mode == SG_THERMAL_HOT
        ? config->hot_output_fps : config->normal_output_fps;
    int length;

    if (sg_format_timestamp(now, timestamp, sizeof(timestamp)) != 0) {
        set_error(error, error_size, "cannot format thermal status timestamp");
        return -1;
    }

    length = snprintf(
        data,
        sizeof(data),
        "{\"enabled\":%s,\"mode\":\"%s\",\"temperature_c\":%.1f,"
        "\"high_temperature_c\":%.1f,\"recovery_temperature_c\":%.1f,"
        "\"camera_width\":%d,\"camera_height\":%d,"
        "\"camera_input_fps\":%d,\"vision_output_fps\":%.2f,"
        "\"timestamp\":\"%s\"}\n",
        config->enabled ? "true" : "false",
        sg_thermal_mode_name(mode),
        temperature_c,
        config->high_temperature_c,
        config->recovery_temperature_c,
        active_width,
        active_height,
        active_input_fps,
        active_output_fps,
        timestamp);
    if (length < 0 || (size_t) length >= sizeof(data)) {
        set_error(error, error_size, "thermal status JSON is too long");
        return -1;
    }

    return atomic_write_if_changed(
        config->status_path,
        data,
        S_IRUSR | S_IWUSR | S_IRGRP,
        &changed,
        error,
        error_size);
}

static int last_emailed_mode(
    const thermal_config_t *config,
    sg_thermal_mode_t *mode,
    bool *present) {
    return read_mode_file(config->last_email_mode_path, mode, present);
}

static bool thermal_email_needed(
    sg_thermal_mode_t mode,
    sg_thermal_mode_t last_mode,
    bool last_mode_present) {
    if (mode == SG_THERMAL_HOT) {
        return !last_mode_present || last_mode != SG_THERMAL_HOT;
    }
    return last_mode_present && last_mode == SG_THERMAL_HOT;
}

static int maybe_send_thermal_email(
    const thermal_config_t *config,
    sg_thermal_mode_t mode,
    double temperature_c) {
    sg_thermal_mode_t previous_email_mode;
    bool previous_email_present;
    time_t last_attempt = 0;
    time_t now = time(NULL);
    char timestamp[SG_TIMESTAMP_SIZE];
    char error[SG_ERROR_SIZE];
    bool state_changed;
    int camera_width;
    int camera_height;
    int camera_input_fps;
    double vision_output_fps;

    if (!config->email_enabled) {
        return 0;
    }
    if (last_emailed_mode(
            config,
            &previous_email_mode,
            &previous_email_present) != 0) {
        fprintf(stderr, "ERROR Cannot read thermal email mode state\n");
        return -1;
    }
    if (!thermal_email_needed(
            mode,
            previous_email_mode,
            previous_email_present)) {
        return 0;
    }

    if (sg_read_epoch_file(
            config->last_email_attempt_path,
            &last_attempt,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR %s\n", error);
        return -1;
    }
    if (!sg_email_due(now, last_attempt, config->email_retry_sec)) {
        return 0;
    }
    if (sg_atomic_write_epoch(
            config->last_email_attempt_path,
            now,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR %s\n", error);
        return -1;
    }
    if (sg_format_timestamp(now, timestamp, sizeof(timestamp)) != 0) {
        fprintf(stderr, "ERROR Cannot format thermal email timestamp\n");
        return -1;
    }

    camera_width = mode == SG_THERMAL_HOT
        ? config->hot_width : config->normal_width;
    camera_height = mode == SG_THERMAL_HOT
        ? config->hot_height : config->normal_height;
    camera_input_fps = mode == SG_THERMAL_HOT
        ? config->hot_input_fps : config->normal_input_fps;
    vision_output_fps = mode == SG_THERMAL_HOT
        ? config->hot_output_fps : config->normal_output_fps;

    if (sg_send_thermal_email(
            &config->shared,
            timestamp,
            temperature_c,
            config->high_temperature_c,
            mode == SG_THERMAL_HOT,
            camera_width,
            camera_height,
            camera_input_fps,
            vision_output_fps,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Thermal event email failed: %s\n", error);
        return -1;
    }

    if (persist_mode(
            config->last_email_mode_path,
            mode,
            &state_changed,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR %s\n", error);
        return -1;
    }
    fprintf(
        stderr,
        "INFO Thermal event email sent for profile %s\n",
        sg_thermal_mode_name(mode));
    return 0;
}

static void sleep_seconds(int seconds) {
    struct timespec delay;

    delay.tv_sec = seconds;
    delay.tv_nsec = 0;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        if (stop_requested) {
            break;
        }
    }
}

static int run_disabled(const thermal_config_t *config) {
    char error[SG_ERROR_SIZE];
    bool override_changed;
    bool state_changed;

    if (synchronize_profile(
            config,
            SG_THERMAL_NORMAL,
            &override_changed,
            error,
            sizeof(error)) != 0 ||
        persist_mode(
            config->state_path,
            SG_THERMAL_NORMAL,
            &state_changed,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "ERROR %s\n", error);
        return 1;
    }

    if (override_changed && config->restart_enabled &&
        restart_vision_service(config->restart_service) != 0) {
        return 1;
    }
    fprintf(stderr, "INFO Thermal manager is disabled; normal profile restored\n");
    return 0;
}

static int run_manager(const thermal_config_t *config) {
    sg_thermal_mode_t mode;
    bool mode_present;
    bool initial_sync = true;
    bool restart_pending = false;
    char error[SG_ERROR_SIZE];

    if (read_mode_file(config->state_path, &mode, &mode_present) != 0) {
        fprintf(stderr, "ERROR Cannot read %s\n", config->state_path);
        return 1;
    }
    if (!mode_present) {
        mode = SG_THERMAL_NORMAL;
    }

    fprintf(
        stderr,
        "INFO Smart Guard thermal manager started: high=%.1fC "
        "recovery=%.1fC poll=%ds\n",
        config->high_temperature_c,
        config->recovery_temperature_c,
        config->poll_interval_sec);
    fprintf(
        stderr,
        "INFO Normal=%dx%d/%d input FPS/%.1f output FPS; "
        "hot=%dx%d/%d input FPS/%.1f output FPS\n",
        config->normal_width,
        config->normal_height,
        config->normal_input_fps,
        config->normal_output_fps,
        config->hot_width,
        config->hot_height,
        config->hot_input_fps,
        config->hot_output_fps);

    while (!stop_requested) {
        sg_thermal_mode_t next_mode;
        double temperature_c;
        bool profile_changed;
        bool override_changed;
        bool state_changed;
        bool restart_needed;

        if (sg_read_cpu_temperature(
                config->shared.cpu_temp_path,
                &temperature_c,
                error,
                sizeof(error)) != 0) {
            fprintf(stderr, "ERROR %s\n", error);
            sleep_seconds(config->poll_interval_sec);
            continue;
        }

        next_mode = sg_thermal_next_mode(
            mode,
            temperature_c,
            config->high_temperature_c,
            config->recovery_temperature_c);
        profile_changed = next_mode != mode;

        if (synchronize_profile(
                config,
                next_mode,
                &override_changed,
                error,
                sizeof(error)) != 0 ||
            persist_mode(
                config->state_path,
                next_mode,
                &state_changed,
                error,
                sizeof(error)) != 0) {
            fprintf(stderr, "ERROR %s\n", error);
            sleep_seconds(config->poll_interval_sec);
            continue;
        }

        restart_needed = restart_pending ||
            profile_changed ||
            override_changed ||
            (initial_sync && next_mode == SG_THERMAL_HOT);
        mode = next_mode;
        initial_sync = false;

        if (profile_changed) {
            fprintf(
                stderr,
                "WARNING Thermal profile changed to %s at %.1f C\n",
                sg_thermal_mode_name(mode),
                temperature_c);
        }

        if (restart_needed && config->restart_enabled) {
            restart_pending =
                restart_vision_service(config->restart_service) != 0;
        } else {
            restart_pending = false;
        }

        if (write_status(
                config,
                mode,
                temperature_c,
                error,
                sizeof(error)) != 0) {
            fprintf(stderr, "ERROR %s\n", error);
        }

        if (!restart_pending) {
            (void) maybe_send_thermal_email(config, mode, temperature_c);
        }
        sleep_seconds(config->poll_interval_sec);
    }

    fprintf(stderr, "INFO Smart Guard thermal manager stopped\n");
    return 0;
}

int main(int argc, char **argv) {
    thermal_config_t config;
    char error[SG_ERROR_SIZE];
    bool check_config = false;
    int status;

    if (argc == 2 && strcmp(argv[1], "--check-config") == 0) {
        check_config = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--check-config]\n", argv[0]);
        return 2;
    }

    if (load_thermal_config(&config, error, sizeof(error)) != 0) {
        fprintf(stderr, "ERROR Configuration: %s\n", error);
        return 1;
    }
    if (check_config) {
        printf(
            "Thermal configuration is valid: high=%.1fC recovery=%.1fC "
            "hot=%dx%d input=%d output=%.1f\n",
            config.high_temperature_c,
            config.recovery_temperature_c,
            config.hot_width,
            config.hot_height,
            config.hot_input_fps,
            config.hot_output_fps);
        return 0;
    }
    if (!config.enabled) {
        return run_disabled(&config);
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    if (config.email_enabled &&
        sg_email_global_init(error, sizeof(error)) != 0) {
        fprintf(stderr, "ERROR %s\n", error);
        return 1;
    }

    status = run_manager(&config);
    if (config.email_enabled) {
        sg_email_global_cleanup();
    }
    return status;
}
