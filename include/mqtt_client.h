#ifndef SMART_GUARD_MQTT_CLIENT_H
#define SMART_GUARD_MQTT_CLIENT_H

#include "smart_guard_notifier.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct sg_mqtt_client sg_mqtt_client_t;

int sg_mqtt_start(
    const sg_config_t *config,
    sg_mqtt_client_t **client_out,
    char *error,
    size_t error_size);

bool sg_mqtt_is_connected(const sg_mqtt_client_t *client);

int sg_mqtt_publish_persons(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size);

int sg_mqtt_publish_telemetry(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size);

int sg_mqtt_publish_alarm(
    sg_mqtt_client_t *client,
    const char *payload,
    char *error,
    size_t error_size);

void sg_mqtt_stop(sg_mqtt_client_t *client);

#endif
