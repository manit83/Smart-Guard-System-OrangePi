#define _POSIX_C_SOURCE 200809L

#include "mqtt_client.h"

#include <mosquitto.h>

#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MQTT_PASSWORD_SIZE 512
#define MQTT_TOPIC_SIZE 512
#define MQTT_STATUS_PAYLOAD_SIZE 512
#define MQTT_TIMESTAMPED_PAYLOAD_SIZE (SG_PAYLOAD_SIZE + 64)

struct sg_mqtt_client {
    struct mosquitto *mosquitto;
    atomic_bool connected;
    atomic_bool stopping;
    atomic_bool shutdown_acknowledged;
    atomic_int shutdown_mid;
    int previous_persons_count;
    int qos;
    char persons_topic[MQTT_TOPIC_SIZE];
    char telemetry_topic[MQTT_TOPIC_SIZE];
    char status_topic[MQTT_TOPIC_SIZE];
    char alarm_topic[MQTT_TOPIC_SIZE];
    char online_payload[MQTT_STATUS_PAYLOAD_SIZE];
    char graceful_offline_payload[MQTT_STATUS_PAYLOAD_SIZE];
};

static void set_error(char *error, size_t size, const char *format, ...) {
    va_list arguments;

    if (error == NULL || size == 0) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static int persons_from_payload(const char *payload) {
    const char *position;
    char *end = NULL;
    long value;

    if (payload == NULL) {
        return -1;
    }

    position = strstr(payload, "\"persons\":");
    if (position == NULL) {
        return -1;
    }

    position += strlen("\"persons\":");
    value = strtol(position, &end, 10);
    if (end == position || value < 0 || value > 1000000L) {
        return -1;
    }
    return (int) value;
}

static int realtime_ms(int64_t *timestamp_ms) {
    struct timespec now;

    if (timestamp_ms == NULL ||
        clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }

    *timestamp_ms =
        (int64_t) now.tv_sec * 1000LL +
        (int64_t) now.tv_nsec / 1000000LL;
    return 0;
}

static int add_sent_timestamp(
    const char *payload,
    int64_t sent_at_ms,
    char *timestamped_payload,
    size_t timestamped_payload_size) {
    size_t length;
    size_t object_start = 0;
    size_t object_end;
    size_t position;
    bool has_members = false;
    int written;

    if (payload == NULL || timestamped_payload == NULL ||
        timestamped_payload_size == 0) {
        return -1;
    }

    length = strlen(payload);
    while (object_start < length &&
           isspace((unsigned char) payload[object_start])) {
        object_start++;
    }

    object_end = length;
    while (object_end > object_start &&
           isspace((unsigned char) payload[object_end - 1])) {
        object_end--;
    }

    if (object_end <= object_start + 1 ||
        payload[object_start] != '{' ||
        payload[object_end - 1] != '}') {
        return -1;
    }

    for (position = object_start + 1;
         position < object_end - 1;
         position++) {
        if (!isspace((unsigned char) payload[position])) {
            has_members = true;
            break;
        }
    }

    written = snprintf(
        timestamped_payload,
        timestamped_payload_size,
        "%.*s%s\"sent_at_ms\":%lld}%s",
        (int) (object_end - 1),
        payload,
        has_members ? "," : "",
        (long long) sent_at_ms,
        payload + object_end);

    return written < 0 ||
        (size_t) written >= timestamped_payload_size ? -1 : 0;
}

static void on_connect(
    struct mosquitto *mosquitto,
    void *userdata,
    int return_code) {
    sg_mqtt_client_t *client = userdata;

    if (return_code == 0) {
        atomic_store(&client->connected, true);
        fprintf(stderr, "INFO MQTT connected; publishing retained online status\n");
        if (mosquitto_publish(
                mosquitto,
                NULL,
                client->status_topic,
                (int) strlen(client->online_payload),
                client->online_payload,
                client->qos,
                true) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "WARNING MQTT online status publish failed\n");
        }
    } else {
        atomic_store(&client->connected, false);
        fprintf(
            stderr,
            "WARNING MQTT connection rejected: %s\n",
            mosquitto_connack_string(return_code));
    }
}

static void on_disconnect(
    struct mosquitto *mosquitto,
    void *userdata,
    int return_code) {
    sg_mqtt_client_t *client = userdata;
    (void) mosquitto;

    atomic_store(&client->connected, false);
    if (!atomic_load(&client->stopping)) {
        fprintf(
            stderr,
            "WARNING MQTT disconnected (%d); automatic reconnect is active\n",
            return_code);
    }
}

static void on_publish(
    struct mosquitto *mosquitto,
    void *userdata,
    int message_id) {
    sg_mqtt_client_t *client = userdata;
    (void) mosquitto;

    if (message_id == atomic_load(&client->shutdown_mid)) {
        atomic_store(&client->shutdown_acknowledged, true);
    }
}

static void on_log(
    struct mosquitto *mosquitto,
    void *userdata,
    int level,
    const char *message) {
    (void) mosquitto;
    (void) userdata;

    if ((level & MOSQ_LOG_ERR) != 0 ||
        (level & MOSQ_LOG_WARNING) != 0) {
        fprintf(stderr, "WARNING MQTT library: %s\n", message);
    }
}

static int build_mqtt_name(
    char *buffer,
    size_t buffer_size,
    const char *prefix,
    const char *student_id,
    const char *suffix) {
    int length = snprintf(
        buffer,
        buffer_size,
        "%s%s%s",
        prefix,
        student_id,
        suffix);
    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

int sg_mqtt_start(
    const sg_config_t *config,
    sg_mqtt_client_t **client_out,
    char *error,
    size_t error_size) {
    sg_mqtt_client_t *client = NULL;
    char password[MQTT_PASSWORD_SIZE];
    char will_payload[MQTT_STATUS_PAYLOAD_SIZE];
    char client_id[MQTT_TOPIC_SIZE];
    int result;
    int length;

    if (config == NULL || client_out == NULL) {
        set_error(error, error_size, "MQTT input is null");
        return -1;
    }
    *client_out = NULL;

    password[0] = '\0';
    if (config->mqtt_username[0] != '\0') {
        if (sg_read_secret(
                config->mqtt_password_file,
                password,
                sizeof(password),
                error,
                error_size) != 0) {
            return -1;
        }
    }

    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        memset(password, 0, sizeof(password));
        set_error(error, error_size, "cannot allocate MQTT client");
        return -1;
    }
    client->previous_persons_count = 0;

    if (build_mqtt_name(
            client->persons_topic,
            sizeof(client->persons_topic),
            "home/",
            config->student_id,
            "/persons") != 0 ||
        build_mqtt_name(
            client->telemetry_topic,
            sizeof(client->telemetry_topic),
            "home/",
            config->student_id,
            "/telemetry") != 0 ||
        build_mqtt_name(
            client->status_topic,
            sizeof(client->status_topic),
            "home/",
            config->student_id,
            "/status") != 0 ||
        build_mqtt_name(
            client->alarm_topic,
            sizeof(client->alarm_topic),
            "home/",
            config->student_id,
            "/alarm") != 0 ||
        build_mqtt_name(
            client_id,
            sizeof(client_id),
            "smart-guard-",
            config->student_id,
            "") != 0) {
        set_error(error, error_size, "MQTT topic or client ID is too long");
        goto fail;
    }
    client->qos = 1;

    length = snprintf(
        will_payload,
        sizeof(will_payload),
        "{\"student_id\":\"%s\",\"status\":\"offline\","
        "\"reason\":\"unexpected_disconnect\"}",
        config->student_id);
    if (length < 0 || (size_t) length >= sizeof(will_payload)) {
        set_error(error, error_size, "MQTT LWT payload is too long");
        goto fail;
    }

    length = snprintf(
        client->online_payload,
        sizeof(client->online_payload),
        "{\"student_id\":\"%s\",\"status\":\"online\"}",
        config->student_id);
    if (length < 0 || (size_t) length >= sizeof(client->online_payload)) {
        set_error(error, error_size, "MQTT online payload is too long");
        goto fail;
    }

    length = snprintf(
        client->graceful_offline_payload,
        sizeof(client->graceful_offline_payload),
        "{\"student_id\":\"%s\",\"status\":\"offline\","
        "\"reason\":\"graceful_shutdown\"}",
        config->student_id);
    if (length < 0 ||
        (size_t) length >= sizeof(client->graceful_offline_payload)) {
        set_error(error, error_size, "MQTT shutdown payload is too long");
        goto fail;
    }

    result = mosquitto_lib_init();
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "mosquitto library init failed: %s",
            mosquitto_strerror(result));
        goto fail;
    }

    client->mosquitto =
        mosquitto_new(client_id, true, client);
    if (client->mosquitto == NULL) {
        set_error(error, error_size, "cannot create MQTT client");
        mosquitto_lib_cleanup();
        goto fail;
    }

    atomic_init(&client->connected, false);
    atomic_init(&client->stopping, false);
    atomic_init(&client->shutdown_acknowledged, false);
    atomic_init(&client->shutdown_mid, -1);

    mosquitto_connect_callback_set(client->mosquitto, on_connect);
    mosquitto_disconnect_callback_set(client->mosquitto, on_disconnect);
    mosquitto_publish_callback_set(client->mosquitto, on_publish);
    mosquitto_log_callback_set(client->mosquitto, on_log);

    result = mosquitto_int_option(
        client->mosquitto,
        MOSQ_OPT_PROTOCOL_VERSION,
        MQTT_PROTOCOL_V311);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "cannot select MQTT 3.1.1: %s",
            mosquitto_strerror(result));
        goto fail_library;
    }

    if (config->mqtt_username[0] != '\0') {
        result = mosquitto_username_pw_set(
            client->mosquitto,
            config->mqtt_username,
            password);
        memset(password, 0, sizeof(password));
        if (result != MOSQ_ERR_SUCCESS) {
            set_error(
                error,
                error_size,
                "cannot configure MQTT credentials: %s",
                mosquitto_strerror(result));
            goto fail_library;
        }
    }
    memset(password, 0, sizeof(password));

    result = mosquitto_will_set(
        client->mosquitto,
        client->status_topic,
        (int) strlen(will_payload),
        will_payload,
        client->qos,
        true);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "cannot configure MQTT LWT: %s",
            mosquitto_strerror(result));
        goto fail_library;
    }

    result = mosquitto_reconnect_delay_set(client->mosquitto, 2, 30, true);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "cannot configure MQTT reconnect: %s",
            mosquitto_strerror(result));
        goto fail_library;
    }

    result = mosquitto_connect_async(
        client->mosquitto,
        config->mqtt_broker_host,
        config->mqtt_broker_port,
        config->mqtt_keepalive_sec);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "cannot start MQTT connection: %s",
            mosquitto_strerror(result));
        goto fail_library;
    }

    result = mosquitto_loop_start(client->mosquitto);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "cannot start MQTT network loop: %s",
            mosquitto_strerror(result));
        goto fail_library;
    }

    *client_out = client;
    return 0;

fail_library:
    memset(password, 0, sizeof(password));
    if (client->mosquitto != NULL) {
        mosquitto_destroy(client->mosquitto);
    }
    mosquitto_lib_cleanup();
fail:
    memset(password, 0, sizeof(password));
    free(client);
    return -1;
}

bool sg_mqtt_is_connected(const sg_mqtt_client_t *client) {
    return client != NULL && atomic_load(&client->connected);
}

static int publish_message(
    sg_mqtt_client_t *client,
    const char *topic,
    const char *payload,
    char *error,
    size_t error_size) {
    int result;
    bool is_persons_message;
    bool add_timestamp = false;
    int persons = -1;
    int64_t sent_at_ms = 0;
    char timestamped_payload[MQTT_TIMESTAMPED_PAYLOAD_SIZE];
    const char *published_payload = payload;

    if (client == NULL || payload == NULL) {
        set_error(error, error_size, "MQTT publish input is null");
        return -1;
    }
    if (!atomic_load(&client->connected)) {
        set_error(error, error_size, "MQTT broker is not connected");
        return 1;
    }

    is_persons_message = strcmp(topic, client->persons_topic) == 0;
    if (is_persons_message) {
        persons = persons_from_payload(payload);
        add_timestamp =
            persons > 0 &&
            persons > client->previous_persons_count;

        if (add_timestamp) {
            if (realtime_ms(&sent_at_ms) != 0) {
                set_error(
                    error,
                    error_size,
                    "cannot read CLOCK_REALTIME for MQTT persons message");
                return -1;
            }
            if (add_sent_timestamp(
                    payload,
                    sent_at_ms,
                    timestamped_payload,
                    sizeof(timestamped_payload)) != 0) {
                set_error(
                    error,
                    error_size,
                    "cannot add sent_at_ms to MQTT persons JSON payload");
                return -1;
            }
            published_payload = timestamped_payload;
        }

        result = mosquitto_publish(
            client->mosquitto,
            NULL,
            topic,
            (int) strlen(published_payload),
            published_payload,
            client->qos,
            false);

        if (result == MOSQ_ERR_SUCCESS) {
            if (persons >= 0) {
                client->previous_persons_count = persons;
            }
            if (add_timestamp) {
                fprintf(
                    stderr,
                    "INFO MQTT persons timestamp: persons=%d "
                    "sent_at_ms=%lld\n",
                    persons,
                    (long long) sent_at_ms);
            }
        }
    } else {
        result = mosquitto_publish(
            client->mosquitto,
            NULL,
            topic,
            (int) strlen(payload),
            payload,
            client->qos,
            false);
    }
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "MQTT QoS %d publish failed: %s",
            client->qos,
            mosquitto_strerror(result));
        return -1;
    }
    return 0;
}

int sg_mqtt_publish_persons(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size) {
    return publish_message(
        client,
        client == NULL ? "" : client->persons_topic,
        payload,
        error,
        error_size);
}

int sg_mqtt_publish_telemetry(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size) {
    return publish_message(
        client,
        client == NULL ? "" : client->telemetry_topic,
        payload,
        error,
        error_size);
}

int sg_mqtt_publish_alarm(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size) {
    return publish_message(
        client,
        client == NULL ? "" : client->alarm_topic,
        payload,
        error,
        error_size);
}

void sg_mqtt_stop(sg_mqtt_client_t *client) {
    if (client == NULL) {
        return;
    }

    atomic_store(&client->stopping, true);
    if (client->mosquitto != NULL) {
        if (atomic_load(&client->connected)) {
            int message_id = -1;
            int result = mosquitto_publish(
                client->mosquitto,
                &message_id,
                client->status_topic,
                (int) strlen(client->graceful_offline_payload),
                client->graceful_offline_payload,
                client->qos,
                true);

            if (result == MOSQ_ERR_SUCCESS) {
                struct timespec delay = {0, 50000000L};
                int attempts;

                atomic_store(&client->shutdown_mid, message_id);
                for (attempts = 0; attempts < 20; attempts++) {
                    if (atomic_load(&client->shutdown_acknowledged)) {
                        break;
                    }
                    nanosleep(&delay, NULL);
                }
            }
            mosquitto_disconnect(client->mosquitto);
        }

        mosquitto_loop_stop(client->mosquitto, false);
        mosquitto_destroy(client->mosquitto);
    }
    mosquitto_lib_cleanup();
    free(client);
}
