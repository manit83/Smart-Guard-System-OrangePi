#define _POSIX_C_SOURCE 200809L

#include "mongoose.h"
#include "guard_state.h"
#include "history_db.h"
#include "telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define URL_SIZE 256
#define PATH_SIZE 512
#define NAME_SIZE 128
#define ID_SIZE 64
#define HOST_SIZE 256
#define JSON_TEXT_SIZE 4096
#define HISTORY_JSON_SIZE 65536
#define MAX_JPEG_SIZE (8U * 1024U * 1024U)
#define STREAM_MAGIC 0x53474d4aU
#define STREAM_SEND_BUFFER_LIMIT (4U * 1024U * 1024U)

typedef struct {
    char bind_ip[64];
    unsigned int http_port;
    unsigned int https_port;
    unsigned int telemetry_interval_sec;
    unsigned int frame_max_age_sec;
    unsigned int mjpeg_stream_fps;
    char student_name[NAME_SIZE];
    char student_id[ID_SIZE];
    char public_host[HOST_SIZE];
    char web_root[PATH_SIZE];
    char frame_path[PATH_SIZE];
    char vision_state_path[PATH_SIZE];
    char guard_state_path[PATH_SIZE];
    char history_db_path[PATH_SIZE];
    char tls_cert_path[PATH_SIZE];
    char tls_key_path[PATH_SIZE];
    char command_request_path[PATH_SIZE];
    unsigned int history_capacity;
    bool guard_default_enabled;
    bool command_reboot_enabled;
} server_config_t;

typedef struct {
    server_config_t config;
    telemetry_sampler_t telemetry;
    struct mg_str tls_certificate;
    struct mg_str tls_key;
} server_context_t;

typedef struct {
    uint64_t last_check_ms;
    int64_t last_mtime_sec;
    int32_t last_mtime_nsec;
    uint32_t frames_remaining;
    uint32_t magic;
} stream_state_t;

_Static_assert(
    sizeof(stream_state_t) <= MG_DATA_SIZE,
    "Mongoose connection data is too small for MJPEG stream state");

static volatile sig_atomic_t running = 1;

static bool method_is(
    const struct mg_http_message *message,
    const char *method);

static void stop_server(int signal_number) {
    (void) signal_number;
    running = 0;
}

static const char *environment_or_default(const char *name,
                                          const char *default_value) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : default_value;
}

static void copy_text(char *destination, size_t size, const char *source) {
    if (size == 0) {
        return;
    }

    snprintf(destination, size, "%s", source != NULL ? source : "");
}

static unsigned int read_port(const char *name, unsigned int default_value) {
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return default_value;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > 65535) {
        fprintf(stderr, "Invalid %s value: %s\n", name, text);
        exit(EXIT_FAILURE);
    }

    return (unsigned int) value;
}

static bool read_boolean(const char *name, bool default_value) {
    const char *text = getenv(name);

    if (text == NULL || text[0] == '\0') {
        return default_value;
    }
    if (strcmp(text, "1") == 0 ||
        strcmp(text, "true") == 0 ||
        strcmp(text, "on") == 0) {
        return true;
    }
    if (strcmp(text, "0") == 0 ||
        strcmp(text, "false") == 0 ||
        strcmp(text, "off") == 0) {
        return false;
    }

    fprintf(stderr, "Invalid %s value: %s\n", name, text);
    exit(EXIT_FAILURE);
}

static void load_config(server_config_t *config) {
    memset(config, 0, sizeof(*config));

    copy_text(
        config->bind_ip,
        sizeof(config->bind_ip),
        environment_or_default("BOARD_BIND_IP", "0.0.0.0"));
    config->http_port = read_port("HTTP_PORT", 80);
    config->https_port = read_port("HTTPS_PORT", 443);
    config->telemetry_interval_sec =
        read_port("TELEMETRY_INTERVAL_SEC", 2);
    config->frame_max_age_sec = read_port("FRAME_MAX_AGE_SEC", 5);
    config->mjpeg_stream_fps = read_port("MJPEG_STREAM_FPS", 2);
    if (config->mjpeg_stream_fps > 30) {
        fprintf(stderr, "MJPEG_STREAM_FPS must be between 1 and 30\n");
        exit(EXIT_FAILURE);
    }
    config->history_capacity = read_port("HISTORY_CAPACITY", 1000);
    config->guard_default_enabled =
        read_boolean("GUARD_DEFAULT_ENABLED", false);

    copy_text(
        config->student_name,
        sizeof(config->student_name),
        environment_or_default("STUDENT_NAME", "Student Name"));
    copy_text(
        config->student_id,
        sizeof(config->student_id),
        environment_or_default("STUDENT_ID", "STUDENT_ID"));
    copy_text(
        config->public_host,
        sizeof(config->public_host),
        environment_or_default("BOARD_PUBLIC_HOST", ""));
    copy_text(
        config->web_root,
        sizeof(config->web_root),
        environment_or_default("WEB_ROOT", "/opt/smart-guard/web"));
    copy_text(
        config->frame_path,
        sizeof(config->frame_path),
        environment_or_default(
            "LATEST_FRAME_PATH", "/run/smart-guard/latest.jpg"));
    copy_text(
        config->vision_state_path,
        sizeof(config->vision_state_path),
        environment_or_default(
            "VISION_STATE_PATH", "/run/smart-guard/vision-state.json"));
    copy_text(
        config->guard_state_path,
        sizeof(config->guard_state_path),
        environment_or_default(
            "GUARD_STATE_PATH", "/var/lib/smart-guard/guard-mode"));
    copy_text(
        config->history_db_path,
        sizeof(config->history_db_path),
        environment_or_default(
            "HISTORY_DB_PATH", "/var/lib/smart-guard/history.db"));
    copy_text(
        config->tls_cert_path,
        sizeof(config->tls_cert_path),
        environment_or_default(
            "TLS_CERT_PATH", "/etc/smart-guard/tls/server.crt"));
    copy_text(
        config->tls_key_path,
        sizeof(config->tls_key_path),
        environment_or_default(
            "TLS_KEY_PATH", "/etc/smart-guard/tls/server.key"));
    copy_text(
        config->command_request_path,
        sizeof(config->command_request_path),
        environment_or_default(
            "COMMAND_REQUEST_PATH",
            "/var/lib/smart-guard/command-request"));
    config->command_reboot_enabled =
        read_boolean("COMMAND_REBOOT_ENABLED", true);
}

static bool frame_is_fresh(const char *path, unsigned int max_age_sec) {
    struct stat info;
    time_t now;

    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return false;
    }

    now = time(NULL);
    if (now == (time_t) -1 || info.st_mtime > now) {
        return true;
    }

    return (unsigned long) (now - info.st_mtime) <= max_age_sec;
}

static int read_person_count(const char *path, int *person_count) {
    FILE *file;
    char buffer[JSON_TEXT_SIZE];
    char *key;
    char *colon;
    char *end = NULL;
    long value;
    size_t bytes_read;

    *person_count = 0;
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[bytes_read] = '\0';

    key = strstr(buffer, "\"persons\"");
    if (key == NULL) {
        key = strstr(buffer, "\"count\"");
    }
    if (key == NULL) {
        return -1;
    }

    colon = strchr(key, ':');
    if (colon == NULL) {
        return -1;
    }

    errno = 0;
    value = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || value < 0 || value > 10000) {
        return -1;
    }

    *person_count = (int) value;
    return 0;
}

static void json_escape(const char *source, char *destination, size_t size) {
    size_t output = 0;

    if (size == 0) {
        return;
    }

    for (size_t input = 0; source[input] != '\0' && output + 1 < size; input++) {
        const unsigned char character = (unsigned char) source[input];
        const char *escape = NULL;

        if (character == '"') {
            escape = "\\\"";
        } else if (character == '\\') {
            escape = "\\\\";
        } else if (character == '\n') {
            escape = "\\n";
        } else if (character == '\r') {
            escape = "\\r";
        } else if (character == '\t') {
            escape = "\\t";
        }

        if (escape != NULL) {
            if (output + 2 >= size) {
                break;
            }
            destination[output++] = escape[0];
            destination[output++] = escape[1];
        } else if (character >= 0x20) {
            destination[output++] = (char) character;
        }
    }

    destination[output] = '\0';
}

static int parse_json_string_member(const struct mg_str body,
                                    const char *member,
                                    char *value,
                                    size_t value_size) {
    char json[512];
    char pattern[96];
    char *cursor;
    size_t output = 0;

    if (body.len == 0 || body.len >= sizeof(json) ||
        member == NULL || value == NULL || value_size < 2) {
        return -1;
    }
    memcpy(json, body.buf, body.len);
    json[body.len] = '\0';
    snprintf(pattern, sizeof(pattern), "\"%s\"", member);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return -1;
    }
    cursor += strlen(pattern);
    while (isspace((unsigned char) *cursor)) {
        cursor++;
    }
    if (*cursor++ != ':') {
        return -1;
    }
    while (isspace((unsigned char) *cursor)) {
        cursor++;
    }
    if (*cursor++ != '"') {
        return -1;
    }

    while (*cursor != '\0' && *cursor != '"') {
        const unsigned char character = (unsigned char) *cursor++;

        if (character == '\\' || character < 0x20 ||
            output + 1 >= value_size) {
            return -1;
        }
        value[output++] = (char) character;
    }
    if (*cursor != '"' || output == 0) {
        return -1;
    }
    value[output] = '\0';
    return 0;
}

static int read_json_string_file_member(const char *path,
                                        const char *member,
                                        char *value,
                                        size_t value_size) {
    FILE *file;
    char buffer[JSON_TEXT_SIZE];
    size_t bytes_read;
    struct mg_str body;

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[bytes_read] = '\0';
    body = mg_str_n(buffer, bytes_read);
    return parse_json_string_member(body, member, value, value_size);
}

static void reply_telemetry(struct mg_connection *connection,
                            struct mg_http_message *message,
                            server_context_t *context) {
    telemetry_sample_t telemetry;
    int person_count = 0;
    bool guard_enabled = false;
    char guard_error[256];
    const bool vision_online =
        frame_is_fresh(
            context->config.vision_state_path,
            context->config.frame_max_age_sec) &&
        read_person_count(context->config.vision_state_path, &person_count) == 0;
    const bool frame_available =
        frame_is_fresh(
            context->config.frame_path, context->config.frame_max_age_sec);
    char student_name[NAME_SIZE * 2];
    char student_id[ID_SIZE * 2];
    char temperature[32];
    char memory[32];
    char cpu[32];
    char uptime[32];

    if (!method_is(message, "GET")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }

    telemetry_read(&context->telemetry, &telemetry);
    if (sg_guard_read(
            context->config.guard_state_path,
            context->config.guard_default_enabled,
            &guard_enabled,
            guard_error,
            sizeof(guard_error)) != 0) {
        guard_enabled = false;
    }
    json_escape(
        context->config.student_name, student_name, sizeof(student_name));
    json_escape(context->config.student_id, student_id, sizeof(student_id));

    if (telemetry.temperature_valid) {
        snprintf(
            temperature, sizeof(temperature), "%.1f", telemetry.cpu_temperature_c);
    } else {
        snprintf(temperature, sizeof(temperature), "null");
    }

    if (telemetry.memory_valid) {
        snprintf(memory, sizeof(memory), "%.1f", telemetry.memory_available_mb);
    } else {
        snprintf(memory, sizeof(memory), "null");
    }

    if (telemetry.cpu_valid) {
        snprintf(cpu, sizeof(cpu), "%.1f", telemetry.cpu_usage_percent);
    } else {
        snprintf(cpu, sizeof(cpu), "null");
    }

    if (telemetry.uptime_seconds >= 0.0) {
        snprintf(uptime, sizeof(uptime), "%.0f", telemetry.uptime_seconds);
    } else {
        snprintf(uptime, sizeof(uptime), "null");
    }

    mg_http_reply(
        connection,
        200,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        "{"
        "\"student_name\":\"%s\","
        "\"student_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"telemetry_interval_sec\":%u,"
        "\"temperature_c\":%s,"
        "\"memory_available_mb\":%s,"
        "\"cpu_percent\":%s,"
        "\"uptime_seconds\":%s,"
        "\"persons\":%d,"
        "\"guard_enabled\":%s,"
        "\"vision_online\":%s,"
        "\"frame_available\":%s"
        "}\n",
        student_name,
        student_id,
        telemetry.timestamp,
        context->config.telemetry_interval_sec,
        temperature,
        memory,
        cpu,
        uptime,
        person_count,
        guard_enabled ? "true" : "false",
        vision_online ? "true" : "false",
        frame_available ? "true" : "false");
}

static void reply_persons(struct mg_connection *connection,
                          struct mg_http_message *message,
                          server_context_t *context) {
    int persons = 0;
    char timestamp[TELEMETRY_TIMESTAMP_SIZE * 2] = "";

    if (!method_is(message, "GET")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }

    if (!frame_is_fresh(
            context->config.vision_state_path,
            context->config.frame_max_age_sec) ||
        read_person_count(context->config.vision_state_path, &persons) != 0 ||
        read_json_string_file_member(
            context->config.vision_state_path,
            "timestamp",
            timestamp,
            sizeof(timestamp)) != 0) {
        mg_http_reply(
            connection,
            503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"vision_not_available\"}\n");
        return;
    }

    mg_http_reply(
        connection,
        200,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        "{\"persons\":%d,\"timestamp\":\"%s\"}\n",
        persons,
        timestamp);
}

static void serve_frame(struct mg_connection *connection,
                        struct mg_http_message *message,
                        server_context_t *context) {
    if (!method_is(message, "GET")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }

    if (!frame_is_fresh(
            context->config.frame_path, context->config.frame_max_age_sec)) {
        mg_http_reply(
            connection,
            404,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"frame_not_available\"}\n");
        return;
    }

    {
        const struct mg_http_serve_opts options = {
            .extra_headers =
                "Cache-Control: no-store, max-age=0\r\n"
                "Pragma: no-cache\r\n"
                "X-Content-Type-Options: nosniff\r\n",
            .mime_types = "jpg=image/jpeg,jpeg=image/jpeg",
            .fs = &mg_fs_posix
        };
        mg_http_serve_file(
            connection, message, context->config.frame_path, &options);
    }
}

static int read_jpeg_file(const char *path,
                          unsigned char **data,
                          size_t *size,
                          struct stat *information) {
    FILE *file;
    unsigned char *buffer;
    size_t bytes_read;

    *data = NULL;
    *size = 0;
    if (stat(path, information) != 0 ||
        !S_ISREG(information->st_mode) ||
        information->st_size <= 0 ||
        (uintmax_t) information->st_size > MAX_JPEG_SIZE) {
        return -1;
    }

    buffer = malloc((size_t) information->st_size);
    if (buffer == NULL) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        free(buffer);
        return -1;
    }
    bytes_read = fread(buffer, 1, (size_t) information->st_size, file);
    fclose(file);
    if (bytes_read != (size_t) information->st_size) {
        free(buffer);
        return -1;
    }

    *data = buffer;
    *size = bytes_read;
    return 0;
}

static int send_mjpeg_frame(struct mg_connection *connection,
                            server_context_t *context,
                            stream_state_t *state,
                            uint64_t now_ms) {
    struct stat information;
    unsigned char *jpeg = NULL;
    size_t jpeg_size = 0;

    if (connection->send.len > STREAM_SEND_BUFFER_LIMIT ||
        now_ms - state->last_check_ms <
            1000U / context->config.mjpeg_stream_fps) {
        return 0;
    }
    state->last_check_ms = now_ms;

    if (read_jpeg_file(
            context->config.frame_path,
            &jpeg,
            &jpeg_size,
            &information) != 0) {
        return -1;
    }
    if ((int64_t) information.st_mtim.tv_sec == state->last_mtime_sec &&
        (int32_t) information.st_mtim.tv_nsec == state->last_mtime_nsec) {
        free(jpeg);
        return 0;
    }

    state->last_mtime_sec = (int64_t) information.st_mtim.tv_sec;
    state->last_mtime_nsec = (int32_t) information.st_mtim.tv_nsec;
    if (mg_printf(
            connection,
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %lu\r\n\r\n",
            (unsigned long) jpeg_size) == 0 ||
        !mg_send(connection, jpeg, jpeg_size) ||
        !mg_send(connection, "\r\n", 2)) {
        free(jpeg);
        return -1;
    }
    free(jpeg);

    if (state->frames_remaining != UINT32_MAX) {
        state->frames_remaining--;
        if (state->frames_remaining == 0) {
            mg_send(connection, "--frame--\r\n", 11);
            connection->is_draining = 1;
        }
    }
    return 1;
}

static void start_mjpeg_stream(struct mg_connection *connection,
                               struct mg_http_message *message,
                               server_context_t *context) {
    stream_state_t *state = (stream_state_t *) connection->data;
    char frames_text[32];
    char *end = NULL;
    unsigned long frames = UINT32_MAX;

    if (!method_is(message, "GET")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }
    if (mg_http_get_var(
            &message->query,
            "frames",
            frames_text,
            sizeof(frames_text)) > 0) {
        errno = 0;
        frames = strtoul(frames_text, &end, 10);
        if (errno != 0 || end == frames_text || *end != '\0' ||
            frames < 1 || frames > 100) {
            mg_http_reply(
                connection,
                400,
                "Content-Type: application/json; charset=utf-8\r\n",
                "{\"error\":\"frames_must_be_1_through_100\"}\n");
            return;
        }
    }
    if (!frame_is_fresh(
            context->config.frame_path,
            context->config.frame_max_age_sec)) {
        mg_http_reply(
            connection,
            503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"stream_not_available\"}\n");
        return;
    }

    memset(state, 0, sizeof(*state));
    state->last_mtime_sec = -1;
    state->last_mtime_nsec = -1;
    state->frames_remaining = (uint32_t) frames;
    state->magic = STREAM_MAGIC;
    connection->is_resp = 1;
    mg_printf(
        connection,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Accel-Buffering: no\r\n"
        "Connection: close\r\n\r\n");
    send_mjpeg_frame(connection, context, state, mg_millis());
}

static bool method_is(
    const struct mg_http_message *message,
    const char *method) {
    return mg_strcmp(message->method, mg_str(method)) == 0;
}

static int parse_guard_enabled(
    const struct mg_str body,
    bool *enabled) {
    char json[256];
    char *key;
    char *colon;

    if (body.len == 0 || body.len >= sizeof(json) || enabled == NULL) {
        return -1;
    }
    memcpy(json, body.buf, body.len);
    json[body.len] = '\0';

    key = strstr(json, "\"enabled\"");
    if (key == NULL || (colon = strchr(key + 9, ':')) == NULL) {
        return -1;
    }
    colon++;
    while (isspace((unsigned char) *colon)) {
        colon++;
    }

    if ((strncmp(colon, "true", 4) == 0 &&
         (colon[4] == '\0' || colon[4] == ',' || colon[4] == '}' ||
          isspace((unsigned char) colon[4]))) ||
        (*colon == '1' &&
         (colon[1] == '\0' || colon[1] == ',' || colon[1] == '}'))) {
        *enabled = true;
        return 0;
    }
    if ((strncmp(colon, "false", 5) == 0 &&
         (colon[5] == '\0' || colon[5] == ',' || colon[5] == '}' ||
          isspace((unsigned char) colon[5]))) ||
        (*colon == '0' &&
         (colon[1] == '\0' || colon[1] == ',' || colon[1] == '}'))) {
        *enabled = false;
        return 0;
    }
    return -1;
}

static void reply_guard_mode(
    struct mg_connection *connection,
    struct mg_http_message *message,
    server_context_t *context) {
    bool enabled;
    char error[256];

    if (method_is(message, "GET")) {
        if (sg_guard_read(
                context->config.guard_state_path,
                context->config.guard_default_enabled,
                &enabled,
                error,
                sizeof(error)) != 0) {
            mg_http_reply(
                connection,
                500,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\n",
                "{\"error\":\"guard_state_unavailable\"}\n");
            return;
        }
    } else if (method_is(message, "POST")) {
        if (parse_guard_enabled(message->body, &enabled) != 0) {
            mg_http_reply(
                connection,
                400,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\n",
                "{\"error\":\"body_must_be_enabled_boolean\"}\n");
            return;
        }
        if (sg_guard_write_atomic(
                context->config.guard_state_path,
                enabled,
                error,
                sizeof(error)) != 0) {
            fprintf(stderr, "Cannot update guard mode: %s\n", error);
            mg_http_reply(
                connection,
                500,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\n",
                "{\"error\":\"guard_state_write_failed\"}\n");
            return;
        }
        fprintf(
            stderr,
            "Guard mode changed through API: %s\n",
            enabled ? "ARMED" : "DISARMED");
    } else {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET, POST\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }

    mg_http_reply(
        connection,
        200,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        "{\"enabled\":%s,\"mode\":\"%s\"}\n",
        enabled ? "true" : "false",
        enabled ? "armed" : "disarmed");
}

static int queue_command_request(const char *request_path,
                                 const char *command,
                                 char *error,
                                 size_t error_size) {
    char temporary_path[PATH_SIZE + 64];
    FILE *file;
    int write_failed = 0;

    if (snprintf(
            temporary_path,
            sizeof(temporary_path),
            "%s.tmp.%ld",
            request_path,
            (long) getpid()) >= (int) sizeof(temporary_path)) {
        snprintf(error, error_size, "command request path is too long");
        return -1;
    }

    file = fopen(temporary_path, "w");
    if (file == NULL) {
        snprintf(
            error,
            error_size,
            "cannot create command request: %s",
            strerror(errno));
        return -1;
    }
    if (fprintf(file, "%s\n", command) < 0 ||
        fflush(file) != 0 ||
        fsync(fileno(file)) != 0) {
        write_failed = 1;
    }
    if (fclose(file) != 0) {
        write_failed = 1;
    }
    if (write_failed) {
        snprintf(
            error,
            error_size,
            "cannot write command request: %s",
            strerror(errno));
        unlink(temporary_path);
        return -1;
    }
    if (rename(temporary_path, request_path) != 0) {
        snprintf(
            error,
            error_size,
            "cannot publish command request: %s",
            strerror(errno));
        unlink(temporary_path);
        return -1;
    }
    return 0;
}

static bool command_is_supported(const char *command) {
    static const char *const supported_commands[] = {
        "reboot"
    };

    for (size_t index = 0;
         index < sizeof(supported_commands) / sizeof(supported_commands[0]);
         index++) {
        if (strcmp(command, supported_commands[index]) == 0) {
            return true;
        }
    }
    return false;
}

static void reply_command(struct mg_connection *connection,
                          struct mg_http_message *message,
                          server_context_t *context) {
    char command[64];
    char error[256];

    if (!method_is(message, "POST")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: POST\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }
    if (parse_json_string_member(
            message->body,
            "cmd",
            command,
            sizeof(command)) != 0) {
        mg_http_reply(
            connection,
            400,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"body_must_contain_cmd_string\"}\n");
        return;
    }
    if (!command_is_supported(command)) {
        mg_http_reply(
            connection,
            422,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"unsupported_command\",\"supported\":[\"reboot\"]}\n");
        return;
    }
    if (strcmp(command, "reboot") == 0 &&
        !context->config.command_reboot_enabled) {
        mg_http_reply(
            connection,
            403,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"reboot_command_disabled\"}\n");
        return;
    }
    if (queue_command_request(
            context->config.command_request_path,
            command,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "Cannot queue API command: %s\n", error);
        mg_http_reply(
            connection,
            503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"command_runner_not_available\"}\n");
        return;
    }

    fprintf(stderr, "Command queued through API: %s\n", command);
    mg_http_reply(
        connection,
        202,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        "{\"status\":\"accepted\",\"cmd\":\"%s\"}\n",
        command);
}

static void reply_history(
    struct mg_connection *connection,
    struct mg_http_message *message,
    server_context_t *context) {
    sg_history_db_t *database = NULL;
    char limit_text[32];
    char error[512];
    char *json;
    char *end = NULL;
    long limit = 5;

    if (!method_is(message, "GET")) {
        mg_http_reply(
            connection,
            405,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Allow: GET\r\n",
            "{\"error\":\"method_not_allowed\"}\n");
        return;
    }

    if (mg_http_get_var(
            &message->query,
            "limit",
            limit_text,
            sizeof(limit_text)) > 0) {
        errno = 0;
        limit = strtol(limit_text, &end, 10);
        if (errno != 0 || end == limit_text || *end != '\0' ||
            limit < 1 || limit > 100) {
            mg_http_reply(
                connection,
                400,
                "Content-Type: application/json; charset=utf-8\r\n",
                "{\"error\":\"limit_must_be_1_through_100\"}\n");
            return;
        }
    }

    if (sg_history_open(
            context->config.history_db_path,
            (int) context->config.history_capacity,
            true,
            &database,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "Cannot open history for API: %s\n", error);
        mg_http_reply(
            connection,
            503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n",
            "{\"error\":\"history_not_available\"}\n");
        return;
    }

    json = malloc(HISTORY_JSON_SIZE);
    if (json == NULL) {
        sg_history_close(database);
        mg_http_reply(
            connection,
            500,
            "Content-Type: application/json; charset=utf-8\r\n",
            "{\"error\":\"out_of_memory\"}\n");
        return;
    }
    if (sg_history_build_json(
            database,
            (int) limit,
            json,
            HISTORY_JSON_SIZE,
            error,
            sizeof(error)) != 0) {
        fprintf(stderr, "Cannot build history API response: %s\n", error);
        free(json);
        sg_history_close(database);
        mg_http_reply(
            connection,
            500,
            "Content-Type: application/json; charset=utf-8\r\n",
            "{\"error\":\"history_query_failed\"}\n");
        return;
    }

    mg_http_reply(
        connection,
        200,
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n",
        "%s",
        json);
    free(json);
    sg_history_close(database);
}

static void https_handler(struct mg_connection *connection,
                          int event,
                          void *event_data) {
    server_context_t *context = (server_context_t *) connection->fn_data;

    if (event == MG_EV_ACCEPT) {
        const struct mg_tls_opts options = {
            .cert = context->tls_certificate,
            .key = context->tls_key
        };
        memset(connection->data, 0, sizeof(connection->data));
        mg_tls_init(connection, &options);
    } else if (event == MG_EV_POLL) {
        stream_state_t *state = (stream_state_t *) connection->data;

        if (state->magic == STREAM_MAGIC && !connection->is_draining) {
            const uint64_t now_ms = *(const uint64_t *) event_data;

            if (send_mjpeg_frame(
                    connection,
                    context,
                    state,
                    now_ms) < 0) {
                connection->is_draining = 1;
            }
        }
    } else if (event == MG_EV_HTTP_MSG) {
        struct mg_http_message *message =
            (struct mg_http_message *) event_data;

        if (mg_match(message->uri, mg_str("/internal/status"), NULL)) {
            reply_telemetry(connection, message, context);
        } else if (mg_match(message->uri, mg_str("/internal/frame"), NULL)) {
            serve_frame(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/stream"), NULL)) {
            start_mjpeg_stream(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/persons"), NULL)) {
            reply_persons(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/telemetry"), NULL)) {
            reply_telemetry(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/command"), NULL)) {
            reply_command(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/guard-mode"), NULL)) {
            reply_guard_mode(connection, message, context);
        } else if (mg_match(
                       message->uri, mg_str("/api/v1/history"), NULL)) {
            reply_history(connection, message, context);
        } else if (mg_match(message->uri, mg_str("/docs"), NULL)) {
            mg_http_reply(
                connection,
                301,
                "Location: /docs/\r\n"
                "Cache-Control: no-store\r\n",
                "");
        } else {
            const struct mg_http_serve_opts options = {
                .root_dir = context->config.web_root,
                .extra_headers =
                    "Cache-Control: no-cache\r\n"
                    "Content-Security-Policy: default-src 'self'; "
                    "script-src 'self'; "
                    "style-src 'self' 'unsafe-inline'; "
                    "img-src 'self' data:; object-src 'none'; "
                    "base-uri 'none'; frame-ancestors 'none'\r\n"
                    "Referrer-Policy: no-referrer\r\n"
                    "X-Content-Type-Options: nosniff\r\n"
                    "X-Frame-Options: DENY\r\n",
                .fs = &mg_fs_posix
            };
            mg_http_serve_dir(connection, message, &options);
        }
    }
}

static bool valid_host_character(char character) {
    const unsigned char value = (unsigned char) character;
    return isalnum(value) || character == '.' || character == '-' ||
           character == ':' || character == '[' || character == ']';
}

static void remove_host_port(char *host) {
    char *closing_bracket;
    char *last_colon;
    char *first_colon;

    if (host[0] == '[') {
        closing_bracket = strchr(host, ']');
        if (closing_bracket != NULL) {
            closing_bracket[1] = '\0';
        }
        return;
    }

    first_colon = strchr(host, ':');
    last_colon = strrchr(host, ':');
    if (first_colon != NULL && first_colon == last_colon) {
        char *cursor = last_colon + 1;
        bool digits_only = *cursor != '\0';

        while (*cursor != '\0') {
            if (!isdigit((unsigned char) *cursor)) {
                digits_only = false;
                break;
            }
            cursor++;
        }

        if (digits_only) {
            *last_colon = '\0';
        }
    }
}

static void select_redirect_host(struct mg_http_message *message,
                                 server_context_t *context,
                                 char *host,
                                 size_t host_size) {
    struct mg_str *header;
    size_t length;

    if (context->config.public_host[0] != '\0') {
        copy_text(host, host_size, context->config.public_host);
    } else {
        header = mg_http_get_header(message, "Host");
        if (header == NULL || header->len == 0) {
            copy_text(host, host_size, "localhost");
        } else {
            length = header->len < host_size - 1 ? header->len : host_size - 1;
            memcpy(host, header->buf, length);
            host[length] = '\0';
        }
    }

    for (size_t index = 0; host[index] != '\0'; index++) {
        if (!valid_host_character(host[index])) {
            copy_text(host, host_size, "localhost");
            break;
        }
    }

    remove_host_port(host);
    if (host[0] == '\0') {
        copy_text(host, host_size, "localhost");
    }
}

static void http_redirect_handler(struct mg_connection *connection,
                                  int event,
                                  void *event_data) {
    server_context_t *context = (server_context_t *) connection->fn_data;

    if (event == MG_EV_HTTP_MSG) {
        struct mg_http_message *message =
            (struct mg_http_message *) event_data;
        char host[HOST_SIZE];
        char port[16] = "";
        char query_prefix[2] = "";
        char location_header[1024];

        select_redirect_host(message, context, host, sizeof(host));
        if (context->config.https_port != 443) {
            snprintf(port, sizeof(port), ":%u", context->config.https_port);
        }
        if (message->query.len > 0) {
            snprintf(query_prefix, sizeof(query_prefix), "?");
        }

        snprintf(
            location_header,
            sizeof(location_header),
            "Location: https://%s%s%.*s%s%.*s\r\n"
            "Cache-Control: no-store\r\n",
            host,
            port,
            (int) message->uri.len,
            message->uri.buf,
            query_prefix,
            (int) message->query.len,
            message->query.buf);

        mg_http_reply(
            connection,
            301,
            location_header,
            "");
    }
}

static int load_tls_material(server_context_t *context) {
    context->tls_certificate =
        mg_file_read(&mg_fs_posix, context->config.tls_cert_path);
    context->tls_key =
        mg_file_read(&mg_fs_posix, context->config.tls_key_path);

    if (context->tls_certificate.buf == NULL ||
        context->tls_certificate.len == 0) {
        fprintf(
            stderr,
            "Cannot read TLS certificate: %s\n",
            context->config.tls_cert_path);
        return -1;
    }

    if (context->tls_key.buf == NULL || context->tls_key.len == 0) {
        fprintf(
            stderr, "Cannot read TLS key: %s\n", context->config.tls_key_path);
        mg_free((void *) context->tls_certificate.buf);
        context->tls_certificate = mg_str_n(NULL, 0);
        return -1;
    }

    return 0;
}

static void release_tls_material(server_context_t *context) {
    mg_free((void *) context->tls_certificate.buf);
    mg_free((void *) context->tls_key.buf);
    context->tls_certificate = mg_str_n(NULL, 0);
    context->tls_key = mg_str_n(NULL, 0);
}

static void configure_logging(void) {
    const char *level = environment_or_default("LOG_LEVEL", "INFO");

    if (strcmp(level, "DEBUG") == 0) {
        mg_log_set(MG_LL_DEBUG);
    } else if (strcmp(level, "VERBOSE") == 0) {
        mg_log_set(MG_LL_VERBOSE);
    } else if (strcmp(level, "ERROR") == 0) {
        mg_log_set(MG_LL_ERROR);
    } else if (strcmp(level, "NONE") == 0) {
        mg_log_set(MG_LL_NONE);
    } else {
        mg_log_set(MG_LL_INFO);
    }
}

int main(void) {
    server_context_t context;
    struct mg_mgr manager;
    char http_url[URL_SIZE];
    char https_url[URL_SIZE];
    struct sigaction signal_action;

    memset(&context, 0, sizeof(context));
    load_config(&context.config);
    telemetry_sampler_init(&context.telemetry);
    configure_logging();

    if (load_tls_material(&context) != 0) {
        return EXIT_FAILURE;
    }

    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = stop_server;
    sigemptyset(&signal_action.sa_mask);
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);

    snprintf(
        http_url,
        sizeof(http_url),
        "http://%s:%u",
        context.config.bind_ip,
        context.config.http_port);
    snprintf(
        https_url,
        sizeof(https_url),
        "https://%s:%u",
        context.config.bind_ip,
        context.config.https_port);

    mg_mgr_init(&manager);
    if (mg_http_listen(
            &manager, http_url, http_redirect_handler, &context) == NULL) {
        fprintf(stderr, "Cannot listen on %s\n", http_url);
        mg_mgr_free(&manager);
        release_tls_material(&context);
        return EXIT_FAILURE;
    }

    if (mg_http_listen(&manager, https_url, https_handler, &context) == NULL) {
        fprintf(stderr, "Cannot listen on %s\n", https_url);
        mg_mgr_free(&manager);
        release_tls_material(&context);
        return EXIT_FAILURE;
    }

    printf(
        "Smart Guard web server started: %s -> %s\n",
        http_url,
        https_url);
    fflush(stdout);

    while (running) {
        mg_mgr_poll(&manager, 200);
    }

    printf("Smart Guard web server stopping\n");
    mg_mgr_free(&manager);
    release_tls_material(&context);
    return EXIT_SUCCESS;
}
