#ifndef SMART_GUARD_GUARD_STATE_H
#define SMART_GUARD_GUARD_STATE_H

#include <stdbool.h>
#include <stddef.h>

int sg_guard_read(
    const char *path,
    bool default_enabled,
    bool *enabled,
    char *error,
    size_t error_size);

int sg_guard_write_atomic(
    const char *path,
    bool enabled,
    char *error,
    size_t error_size);

#endif
