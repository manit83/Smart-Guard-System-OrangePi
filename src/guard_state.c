#define _POSIX_C_SOURCE 200809L

#include "guard_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define GUARD_VALUE_SIZE 32
#define GUARD_PATH_SIZE 1024

static void set_error(char *error, size_t size, const char *format, ...) {
    va_list arguments;

    if (error == NULL || size == 0) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static int write_all(int descriptor, const char *data, size_t length) {
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(descriptor, data + written, length - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t) result;
    }
    return 0;
}

int sg_guard_read(
    const char *path,
    bool default_enabled,
    bool *enabled,
    char *error,
    size_t error_size) {
    FILE *file;
    char value[GUARD_VALUE_SIZE];

    if (path == NULL || path[0] != '/' || enabled == NULL) {
        set_error(error, error_size, "guard state input is invalid");
        return -1;
    }

    *enabled = default_enabled;
    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        set_error(
            error,
            error_size,
            "cannot open guard state %s: %s",
            path,
            strerror(errno));
        return -1;
    }

    if (fgets(value, sizeof(value), file) == NULL) {
        fclose(file);
        set_error(error, error_size, "guard state %s is empty", path);
        return -1;
    }
    fclose(file);
    value[strcspn(value, "\r\n \t")] = '\0';

    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "on") == 0) {
        *enabled = true;
        return 0;
    }
    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0) {
        *enabled = false;
        return 0;
    }

    set_error(error, error_size, "guard state %s must contain 0 or 1", path);
    return -1;
}

int sg_guard_write_atomic(
    const char *path,
    bool enabled,
    char *error,
    size_t error_size) {
    char temporary[GUARD_PATH_SIZE];
    const char *value = enabled ? "1\n" : "0\n";
    int descriptor;
    int length;
    int saved_errno;

    if (path == NULL || path[0] != '/') {
        set_error(error, error_size, "guard state path must be absolute");
        return -1;
    }

    length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t) length >= sizeof(temporary)) {
        set_error(error, error_size, "guard state path is too long");
        return -1;
    }

    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        set_error(
            error,
            error_size,
            "cannot create guard state beside %s: %s",
            path,
            strerror(errno));
        return -1;
    }

    if (fchmod(descriptor, 0660) != 0 ||
        write_all(descriptor, value, 2) != 0 ||
        fsync(descriptor) != 0) {
        saved_errno = errno;
        close(descriptor);
        unlink(temporary);
        set_error(
            error,
            error_size,
            "cannot write guard state %s: %s",
            path,
            strerror(saved_errno));
        return -1;
    }

    if (close(descriptor) != 0) {
        saved_errno = errno;
        unlink(temporary);
        set_error(
            error,
            error_size,
            "cannot close guard state %s: %s",
            path,
            strerror(saved_errno));
        return -1;
    }

    if (rename(temporary, path) != 0) {
        saved_errno = errno;
        unlink(temporary);
        set_error(
            error,
            error_size,
            "cannot replace guard state %s: %s",
            path,
            strerror(saved_errno));
        return -1;
    }

    return 0;
}
