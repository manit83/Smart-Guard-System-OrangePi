#define _POSIX_C_SOURCE 200809L

#include "mqtt_client.h"

#include <mosquitto.h>

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

struct sg_mqtt_client {
    struct mosquitto *mosquitto;
    atomic_bool connected;
    atomic_bool stopping;
    atomic_bool shutdown_acknowledged;
    atomic_int shutdown_mid;
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
                1,
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

static int format_topic(
    char *buffer,
    size_t buffer_size,
    const char *student_id,
    const char *leaf) {
    int length = snprintf(
        buffer,
        buffer_size,
        "home/%s/%s",
        student_id,
        leaf);
    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

static int format_alarm_topic(
    char *buffer,
    size_t buffer_size,
    const char *student_id) {
    int length = snprintf(
        buffer,
        buffer_size,
        "alarm/%s/home",
        student_id);
    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

int sg_mqtt_start(
    const sg_config_t *config,
    sg_mqtt_client_t **client_out,
    char *error,
    size_t error_size) {
    sg_mqtt_client_t *client = NULL;
    char password[MQTT_PASSWORD_SIZE];
    char client_id[SG_TEXT_SIZE + 64];
    char will_payload[MQTT_STATUS_PAYLOAD_SIZE];
    int result;
    int length;

    if (config == NULL || client_out == NULL) {
        set_error(error, error_size, "MQTT input is null");
        return -1;
    }
    *client_out = NULL;

    if (sg_read_secret(
            config->mqtt_password_file,
            password,
            sizeof(password),
            error,
            error_size) != 0) {
        return -1;
    }

    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        memset(password, 0, sizeof(password));
        set_error(error, error_size, "cannot allocate MQTT client");
        return -1;
    }

    if (format_topic(
            client->persons_topic,
            sizeof(client->persons_topic),
            config->student_id,
            "persons") != 0 ||
        format_topic(
            client->telemetry_topic,
            sizeof(client->telemetry_topic),
            config->student_id,
            "telemetry") != 0 ||
        format_topic(
            client->status_topic,
            sizeof(client->status_topic),
            config->student_id,
            "status") != 0 ||
        format_alarm_topic(
            client->alarm_topic,
            sizeof(client->alarm_topic),
            config->student_id) != 0) {
        set_error(error, error_size, "MQTT topic is too long");
        goto fail;
    }

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

    length = snprintf(
        client_id,
        sizeof(client_id),
        "smart-guard-%s-%ld",
        config->student_id,
        (long) getpid());
    if (length < 0 || (size_t) length >= sizeof(client_id)) {
        set_error(error, error_size, "MQTT client ID is too long");
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

    client->mosquitto = mosquitto_new(client_id, true, client);
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

    result = mosquitto_will_set(
        client->mosquitto,
        client->status_topic,
        (int) strlen(will_payload),
        will_payload,
        1,
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

static int publish_qos1(
    sg_mqtt_client_t *client,
    const char *topic,
    const char *payload,
    char *error,
    size_t error_size) {
    int result;

    if (client == NULL || payload == NULL) {
        set_error(error, error_size, "MQTT publish input is null");
        return -1;
    }
    if (!atomic_load(&client->connected)) {
        set_error(error, error_size, "MQTT broker is not connected");
        return 1;
    }

    result = mosquitto_publish(
        client->mosquitto,
        NULL,
        topic,
        (int) strlen(payload),
        payload,
        1,
        false);
    if (result != MOSQ_ERR_SUCCESS) {
        set_error(
            error,
            error_size,
            "MQTT QoS 1 publish failed: %s",
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
    return publish_qos1(
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
    return publish_qos1(
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
    return publish_qos1(
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
                1,
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
