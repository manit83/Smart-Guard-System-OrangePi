#ifndef TEST_STUB_MOSQUITTO_H
#define TEST_STUB_MOSQUITTO_H

#include <stdbool.h>

struct mosquitto;

#define MOSQ_ERR_SUCCESS 0
#define MOSQ_LOG_ERR 0x08
#define MOSQ_LOG_WARNING 0x04
#define MOSQ_OPT_PROTOCOL_VERSION 1
#define MQTT_PROTOCOL_V311 4

int mosquitto_lib_init(void);
int mosquitto_lib_cleanup(void);
struct mosquitto *mosquitto_new(
    const char *client_id,
    bool clean_session,
    void *userdata);
void mosquitto_destroy(struct mosquitto *mosquitto);
const char *mosquitto_strerror(int error_code);
const char *mosquitto_connack_string(int return_code);

void mosquitto_connect_callback_set(
    struct mosquitto *mosquitto,
    void (*callback)(struct mosquitto *, void *, int));
void mosquitto_disconnect_callback_set(
    struct mosquitto *mosquitto,
    void (*callback)(struct mosquitto *, void *, int));
void mosquitto_publish_callback_set(
    struct mosquitto *mosquitto,
    void (*callback)(struct mosquitto *, void *, int));
void mosquitto_log_callback_set(
    struct mosquitto *mosquitto,
    void (*callback)(struct mosquitto *, void *, int, const char *));

int mosquitto_int_option(
    struct mosquitto *mosquitto,
    int option,
    int value);
int mosquitto_username_pw_set(
    struct mosquitto *mosquitto,
    const char *username,
    const char *password);
int mosquitto_will_set(
    struct mosquitto *mosquitto,
    const char *topic,
    int payload_length,
    const void *payload,
    int qos,
    bool retain);
int mosquitto_reconnect_delay_set(
    struct mosquitto *mosquitto,
    unsigned int delay,
    unsigned int delay_max,
    bool exponential_backoff);
int mosquitto_connect_async(
    struct mosquitto *mosquitto,
    const char *host,
    int port,
    int keepalive);
int mosquitto_loop_start(struct mosquitto *mosquitto);
int mosquitto_loop_stop(struct mosquitto *mosquitto, bool force);
int mosquitto_disconnect(struct mosquitto *mosquitto);
int mosquitto_publish(
    struct mosquitto *mosquitto,
    int *message_id,
    const char *topic,
    int payload_length,
    const void *payload,
    int qos,
    bool retain);

#endif
