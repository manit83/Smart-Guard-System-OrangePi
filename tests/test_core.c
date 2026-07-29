#define _POSIX_C_SOURCE 200809L

#include "smart_guard_notifier.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_json_parser(void) {
    const char *valid =
        "{\"persons\":2,\"faces\":2,"
        "\"timestamp\":\"2026-07-27T10:20:30+04:00\",\"fps\":2.0}";
    sg_vision_state_t state;
    char error[SG_ERROR_SIZE];

    CHECK(
        sg_parse_vision_state_json(valid, &state, error, sizeof(error)) == 0,
        "valid vision JSON parses");
    CHECK(state.persons == 2, "persons is extracted");
    CHECK(
        strcmp(state.timestamp, "2026-07-27T10:20:30+04:00") == 0,
        "timestamp is extracted");

    CHECK(
        sg_parse_vision_state_json(
            "{\"persons\":-1,\"timestamp\":\"bad\"}",
            &state,
            error,
            sizeof(error)) != 0,
        "negative persons is rejected");
    CHECK(
        sg_parse_vision_state_json(
            "{\"persons\":1}",
            &state,
            error,
            sizeof(error)) != 0,
        "missing timestamp is rejected");
}

static void test_payload(void) {
    sg_config_t config;
    sg_vision_state_t state;
    char payload[SG_PAYLOAD_SIZE];

    memset(&config, 0, sizeof(config));
    snprintf(config.student_id, sizeof(config.student_id), "402000000");
    state.persons = 3;
    snprintf(
        state.timestamp,
        sizeof(state.timestamp),
        "2026-07-27T10:20:30+04:00");

    CHECK(
        sg_build_event_payload(
            &config,
            &state,
            51.25,
            payload,
            sizeof(payload)) == 0,
        "MQTT JSON payload builds");
    CHECK(
        strcmp(
            payload,
            "{\"student_id\":\"402000000\",\"persons\":3,"
            "\"temperature_c\":51.2,"
            "\"timestamp\":\"2026-07-27T10:20:30+04:00\"}") == 0,
        "MQTT JSON contains all required values");
}

static void test_debounce(void) {
    CHECK(sg_email_due(100, 0, 30), "first email is due");
    CHECK(!sg_email_due(129, 100, 30), "29 seconds is blocked");
    CHECK(sg_email_due(130, 100, 30), "30 seconds is allowed");
    CHECK(!sg_email_due(90, 100, 30), "backward clock is blocked");
}

static void test_temperature_file(void) {
    char path[] = "/tmp/smart-guard-temperature-XXXXXX";
    int descriptor = mkstemp(path);
    const char data[] = "48750\n";
    double temperature = 0.0;
    char error[SG_ERROR_SIZE];

    CHECK(descriptor >= 0, "temporary temperature file is created");
    if (descriptor < 0) {
        return;
    }
    CHECK(
        write(descriptor, data, sizeof(data) - 1) ==
            (ssize_t) (sizeof(data) - 1),
        "temperature value is written");
    close(descriptor);

    CHECK(
        sg_read_cpu_temperature(
            path,
            &temperature,
            error,
            sizeof(error)) == 0,
        "temperature file is read directly");
    CHECK(
        temperature > 48.74 && temperature < 48.76,
        "millidegrees are converted to Celsius");
    unlink(path);
}

static void test_persistent_epoch(void) {
    char directory[] = "/tmp/smart-guard-state-XXXXXX";
    char *created_directory;
    char path[256];
    char error[SG_ERROR_SIZE];
    time_t value = 0;

    created_directory = mkdtemp(directory);
    CHECK(created_directory != NULL, "temporary state directory is created");
    if (created_directory == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s/last-email", directory);

    CHECK(
        sg_atomic_write_epoch(path, (time_t) 123456, error, sizeof(error)) == 0,
        "debounce epoch is persisted atomically");
    CHECK(
        sg_read_epoch_file(path, &value, error, sizeof(error)) == 0,
        "debounce epoch is read");
    CHECK(value == (time_t) 123456, "debounce epoch round-trips");

    unlink(path);
    rmdir(directory);
}

static void test_config_minimum_debounce(void) {
    sg_config_t config;
    char error[SG_ERROR_SIZE];

    setenv("STUDENT_ID", "402000000", 1);
    setenv("EMAIL_ENABLED", "1", 1);
    setenv("EMAIL_TO", "receiver@example.com", 1);
    setenv("SMTP_HOST", "smtp.example.com", 1);
    setenv("SMTP_USERNAME", "sender@example.com", 1);
    setenv("SMTP_FROM", "sender@example.com", 1);
    setenv("EMAIL_DEBOUNCE_SEC", "29", 1);
    setenv("MQTT_ENABLED", "0", 1);

    CHECK(
        sg_load_config(&config, error, sizeof(error)) != 0,
        "debounce below 30 seconds is rejected");

    setenv("EMAIL_DEBOUNCE_SEC", "30", 1);
    CHECK(
        sg_load_config(&config, error, sizeof(error)) == 0,
        "30-second debounce is accepted");
}

int main(void) {
    test_json_parser();
    test_payload();
    test_debounce();
    test_temperature_file();
    test_persistent_epoch();
    test_config_minimum_debounce();

    if (failures != 0) {
        fprintf(stderr, "%d core test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("All Smart Guard notifier core tests passed.\n");
    return EXIT_SUCCESS;
}
