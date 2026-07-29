#define _POSIX_C_SOURCE 200809L

#include "history_db.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct sg_history_db {
    sqlite3 *handle;
    int capacity;
    bool read_only;
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

static int execute_sql(
    sqlite3 *database,
    const char *sql,
    char *error,
    size_t error_size) {
    char *sqlite_error = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &sqlite_error);

    if (result != SQLITE_OK) {
        set_error(
            error,
            error_size,
            "SQLite error: %s",
            sqlite_error != NULL ? sqlite_error : sqlite3_errmsg(database));
        sqlite3_free(sqlite_error);
        return -1;
    }
    return 0;
}

static int prepare(
    sqlite3 *database,
    const char *sql,
    sqlite3_stmt **statement,
    char *error,
    size_t error_size) {
    int result = sqlite3_prepare_v2(database, sql, -1, statement, NULL);

    if (result != SQLITE_OK) {
        set_error(
            error,
            error_size,
            "SQLite prepare failed: %s",
            sqlite3_errmsg(database));
        return -1;
    }
    return 0;
}

static int initialize_schema(
    sg_history_db_t *database,
    char *error,
    size_t error_size) {
    static const char *schema =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS history_meta ("
        "  id INTEGER PRIMARY KEY CHECK(id = 1),"
        "  total_events INTEGER NOT NULL,"
        "  total_persons INTEGER NOT NULL,"
        "  capacity INTEGER NOT NULL"
        ");"
        "INSERT OR IGNORE INTO history_meta"
        "  (id, total_events, total_persons, capacity)"
        "  VALUES (1, 0, 0, 1);"
        "CREATE TABLE IF NOT EXISTS detections ("
        "  slot INTEGER PRIMARY KEY,"
        "  sequence INTEGER NOT NULL UNIQUE,"
        "  detected_at TEXT NOT NULL,"
        "  persons INTEGER NOT NULL CHECK(persons > 0),"
        "  frame_path TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS detections_sequence_desc"
        "  ON detections(sequence DESC);"
        "CREATE UNIQUE INDEX IF NOT EXISTS detections_timestamp_unique"
        "  ON detections(detected_at);"
        "COMMIT;";
    sqlite3_stmt *statement = NULL;
    int result;

    if (execute_sql(database->handle, schema, error, error_size) != 0) {
        execute_sql(database->handle, "ROLLBACK;", NULL, 0);
        return -1;
    }

    if (prepare(
            database->handle,
            "UPDATE history_meta SET capacity = ? WHERE id = 1;",
            &statement,
            error,
            error_size) != 0) {
        return -1;
    }
    sqlite3_bind_int(statement, 1, database->capacity);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "cannot store SQLite history capacity: %s",
            sqlite3_errmsg(database->handle));
        return -1;
    }

    if (prepare(
            database->handle,
            "DELETE FROM detections WHERE slot >= ?;",
            &statement,
            error,
            error_size) != 0) {
        return -1;
    }
    sqlite3_bind_int(statement, 1, database->capacity);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "cannot resize SQLite history ring: %s",
            sqlite3_errmsg(database->handle));
        return -1;
    }
    return 0;
}

int sg_history_open(
    const char *path,
    int capacity,
    bool read_only,
    sg_history_db_t **database,
    char *error,
    size_t error_size) {
    sg_history_db_t *opened;
    int flags;
    int result;

    if (path == NULL || path[0] != '/' || database == NULL ||
        capacity < 5 || capacity > 100000) {
        set_error(error, error_size, "SQLite history configuration is invalid");
        return -1;
    }
    *database = NULL;

    opened = calloc(1, sizeof(*opened));
    if (opened == NULL) {
        set_error(error, error_size, "cannot allocate SQLite history handle");
        return -1;
    }
    opened->capacity = capacity;
    opened->read_only = read_only;
    flags = read_only
        ? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
        : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    result = sqlite3_open_v2(path, &opened->handle, flags, NULL);
    if (result != SQLITE_OK) {
        set_error(
            error,
            error_size,
            "cannot open SQLite history %s: %s",
            path,
            opened->handle != NULL
                ? sqlite3_errmsg(opened->handle)
                : sqlite3_errstr(result));
        if (opened->handle != NULL) {
            sqlite3_close(opened->handle);
        }
        free(opened);
        return -1;
    }

    sqlite3_busy_timeout(opened->handle, 2000);
    if (!read_only) {
        if (execute_sql(
                opened->handle,
                "PRAGMA journal_mode=DELETE;"
                "PRAGMA synchronous=FULL;"
                "PRAGMA temp_store=MEMORY;",
                error,
                error_size) != 0 ||
            initialize_schema(opened, error, error_size) != 0) {
            sqlite3_close(opened->handle);
            free(opened);
            return -1;
        }
        if (chmod(path, 0640) != 0) {
            set_error(
                error,
                error_size,
                "cannot set permissions on %s: %s",
                path,
                strerror(errno));
            sqlite3_close(opened->handle);
            free(opened);
            return -1;
        }
    }

    *database = opened;
    return 0;
}

void sg_history_close(sg_history_db_t *database) {
    if (database == NULL) {
        return;
    }
    sqlite3_close(database->handle);
    free(database);
}

int sg_history_record(
    sg_history_db_t *database,
    const char *timestamp,
    int persons,
    int newly_detected_persons,
    const char *frame_path,
    char *error,
    size_t error_size) {
    sqlite3_stmt *read_meta = NULL;
    sqlite3_stmt *find_duplicate = NULL;
    sqlite3_stmt *write_meta = NULL;
    sqlite3_stmt *write_event = NULL;
    sqlite3_int64 total_events;
    sqlite3_int64 total_persons;
    sqlite3_int64 sequence;
    int slot;
    int result;

    if (database == NULL || database->read_only || timestamp == NULL ||
        timestamp[0] == '\0' || frame_path == NULL || persons <= 0) {
        set_error(error, error_size, "SQLite history record input is invalid");
        return -1;
    }

    if (execute_sql(
            database->handle, "BEGIN IMMEDIATE;", error, error_size) != 0) {
        return -1;
    }

    if (prepare(
            database->handle,
            "SELECT 1 FROM detections WHERE detected_at = ? LIMIT 1;",
            &find_duplicate,
            error,
            error_size) != 0) {
        goto rollback;
    }
    sqlite3_bind_text(
        find_duplicate, 1, timestamp, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(find_duplicate);
    sqlite3_finalize(find_duplicate);
    find_duplicate = NULL;
    if (result == SQLITE_ROW) {
        execute_sql(database->handle, "ROLLBACK;", NULL, 0);
        return 1;
    }
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "cannot check duplicate SQLite history event: %s",
            sqlite3_errmsg(database->handle));
        goto rollback;
    }

    if (prepare(
            database->handle,
            "SELECT total_events, total_persons"
            " FROM history_meta WHERE id = 1;",
            &read_meta,
            error,
            error_size) != 0) {
        goto rollback;
    }
    result = sqlite3_step(read_meta);
    if (result != SQLITE_ROW) {
        set_error(
            error,
            error_size,
            "SQLite history metadata is missing: %s",
            sqlite3_errmsg(database->handle));
        goto rollback;
    }
    total_events = sqlite3_column_int64(read_meta, 0);
    total_persons = sqlite3_column_int64(read_meta, 1);
    sqlite3_finalize(read_meta);
    read_meta = NULL;

    sequence = total_events + 1;
    slot = (int) ((sequence - 1) % database->capacity);

    if (prepare(
            database->handle,
            "UPDATE history_meta"
            " SET total_events = ?, total_persons = ?, capacity = ?"
            " WHERE id = 1;",
            &write_meta,
            error,
            error_size) != 0) {
        goto rollback;
    }
    sqlite3_bind_int64(write_meta, 1, sequence);
    if (newly_detected_persons < 1 || newly_detected_persons > persons) {
        set_error(
            error,
            error_size,
            "newly detected person count is invalid");
        goto rollback;
    }
    sqlite3_bind_int64(
        write_meta, 2, total_persons + newly_detected_persons);
    sqlite3_bind_int(write_meta, 3, database->capacity);
    result = sqlite3_step(write_meta);
    sqlite3_finalize(write_meta);
    write_meta = NULL;
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "cannot update SQLite history totals: %s",
            sqlite3_errmsg(database->handle));
        goto rollback;
    }

    if (prepare(
            database->handle,
            "INSERT OR REPLACE INTO detections"
            " (slot, sequence, detected_at, persons, frame_path)"
            " VALUES (?, ?, ?, ?, ?);",
            &write_event,
            error,
            error_size) != 0) {
        goto rollback;
    }
    sqlite3_bind_int(write_event, 1, slot);
    sqlite3_bind_int64(write_event, 2, sequence);
    sqlite3_bind_text(write_event, 3, timestamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(write_event, 4, persons);
    sqlite3_bind_text(write_event, 5, frame_path, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(write_event);
    sqlite3_finalize(write_event);
    write_event = NULL;
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "cannot insert SQLite history event: %s",
            sqlite3_errmsg(database->handle));
        goto rollback;
    }

    if (execute_sql(database->handle, "COMMIT;", error, error_size) != 0) {
        execute_sql(database->handle, "ROLLBACK;", NULL, 0);
        return -1;
    }
    return 0;

rollback:
    sqlite3_finalize(find_duplicate);
    sqlite3_finalize(read_meta);
    sqlite3_finalize(write_meta);
    sqlite3_finalize(write_event);
    execute_sql(database->handle, "ROLLBACK;", NULL, 0);
    return -1;
}

static int append_text(
    char *buffer,
    size_t size,
    size_t *used,
    const char *format,
    ...) {
    va_list arguments;
    int length;

    if (*used >= size) {
        return -1;
    }

    va_start(arguments, format);
    length = vsnprintf(buffer + *used, size - *used, format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t) length >= size - *used) {
        return -1;
    }
    *used += (size_t) length;
    return 0;
}

static int append_json_string(
    char *buffer,
    size_t size,
    size_t *used,
    const unsigned char *text) {
    size_t index;

    if (append_text(buffer, size, used, "\"") != 0) {
        return -1;
    }
    if (text != NULL) {
        for (index = 0; text[index] != '\0'; index++) {
            unsigned char character = text[index];

            if (character == '"' || character == '\\') {
                if (append_text(
                        buffer, size, used, "\\%c", (char) character) != 0) {
                    return -1;
                }
            } else if (character == '\n') {
                if (append_text(buffer, size, used, "\\n") != 0) {
                    return -1;
                }
            } else if (character == '\r') {
                if (append_text(buffer, size, used, "\\r") != 0) {
                    return -1;
                }
            } else if (character == '\t') {
                if (append_text(buffer, size, used, "\\t") != 0) {
                    return -1;
                }
            } else if (character >= 0x20) {
                if (append_text(
                        buffer, size, used, "%c", (char) character) != 0) {
                    return -1;
                }
            }
        }
    }
    return append_text(buffer, size, used, "\"");
}

int sg_history_build_json(
    sg_history_db_t *database,
    int limit,
    char *json,
    size_t json_size,
    char *error,
    size_t error_size) {
    sqlite3_stmt *metadata = NULL;
    sqlite3_stmt *events = NULL;
    sqlite3_int64 total_events;
    sqlite3_int64 total_persons;
    sqlite3_int64 stored_records;
    int capacity;
    int result;
    bool first = true;
    size_t used = 0;

    if (database == NULL || json == NULL || json_size < 128 ||
        limit < 1 || limit > 100) {
        set_error(error, error_size, "SQLite history query input is invalid");
        return -1;
    }

    if (prepare(
            database->handle,
            "SELECT total_events, total_persons, capacity,"
            " (SELECT COUNT(*) FROM detections)"
            " FROM history_meta WHERE id = 1;",
            &metadata,
            error,
            error_size) != 0) {
        return -1;
    }
    result = sqlite3_step(metadata);
    if (result != SQLITE_ROW) {
        set_error(
            error,
            error_size,
            "SQLite history metadata is unavailable: %s",
            sqlite3_errmsg(database->handle));
        sqlite3_finalize(metadata);
        return -1;
    }
    total_events = sqlite3_column_int64(metadata, 0);
    total_persons = sqlite3_column_int64(metadata, 1);
    capacity = sqlite3_column_int(metadata, 2);
    stored_records = sqlite3_column_int64(metadata, 3);
    sqlite3_finalize(metadata);

    if (append_text(
            json,
            json_size,
            &used,
            "{\"total_detection_events\":%lld,"
            "\"total_humans_detected\":%lld,"
            "\"stored_records\":%lld,"
            "\"capacity\":%d,\"events\":[",
            (long long) total_events,
            (long long) total_persons,
            (long long) stored_records,
            capacity) != 0) {
        set_error(error, error_size, "history JSON buffer is too small");
        return -1;
    }

    if (prepare(
            database->handle,
            "SELECT sequence, detected_at, persons, frame_path"
            " FROM detections ORDER BY sequence DESC LIMIT ?;",
            &events,
            error,
            error_size) != 0) {
        return -1;
    }
    sqlite3_bind_int(events, 1, limit);

    while ((result = sqlite3_step(events)) == SQLITE_ROW) {
        const unsigned char *timestamp = sqlite3_column_text(events, 1);
        const unsigned char *frame_path = sqlite3_column_text(events, 3);

        if ((!first && append_text(json, json_size, &used, ",") != 0) ||
            append_text(
                json,
                json_size,
                &used,
                "{\"sequence\":%lld,\"timestamp\":",
                (long long) sqlite3_column_int64(events, 0)) != 0 ||
            append_json_string(json, json_size, &used, timestamp) != 0 ||
            append_text(
                json,
                json_size,
                &used,
                ",\"persons\":%d,\"frame_path\":",
                sqlite3_column_int(events, 2)) != 0 ||
            append_json_string(json, json_size, &used, frame_path) != 0 ||
            append_text(json, json_size, &used, "}") != 0) {
            sqlite3_finalize(events);
            set_error(error, error_size, "history JSON buffer is too small");
            return -1;
        }
        first = false;
    }
    sqlite3_finalize(events);
    if (result != SQLITE_DONE) {
        set_error(
            error,
            error_size,
            "SQLite history query failed: %s",
            sqlite3_errmsg(database->handle));
        return -1;
    }

    if (append_text(json, json_size, &used, "]}\n") != 0) {
        set_error(error, error_size, "history JSON buffer is too small");
        return -1;
    }
    return 0;
}
