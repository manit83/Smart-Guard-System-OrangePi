#ifndef SMART_GUARD_HISTORY_DB_H
#define SMART_GUARD_HISTORY_DB_H

#include <stdbool.h>
#include <stddef.h>

typedef struct sg_history_db sg_history_db_t;

int sg_history_open(
    const char *path,
    int capacity,
    bool read_only,
    sg_history_db_t **database,
    char *error,
    size_t error_size);

void sg_history_close(sg_history_db_t *database);

int sg_history_record(
    sg_history_db_t *database,
    const char *timestamp,
    int persons,
    int newly_detected_persons,
    const char *frame_path,
    char *error,
    size_t error_size);

int sg_history_build_json(
    sg_history_db_t *database,
    int limit,
    char *json,
    size_t json_size,
    char *error,
    size_t error_size);

#endif
