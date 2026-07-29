#define _POSIX_C_SOURCE 200809L

#include "history_db.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/smart-guard-history-test.XXXXXX";
    char path[512];
    char error[512];
    char json[8192];
    sg_history_db_t *database = NULL;
    int index;

    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/history.db", directory);

    assert(sg_history_open(
               path, 5, false, &database, error, sizeof(error)) == 0);
    for (index = 1; index <= 7; index++) {
        char timestamp[64];

        snprintf(
            timestamp,
            sizeof(timestamp),
            "2026-07-27T20:00:%02d+04:00",
            index);
        assert(sg_history_record(
                   database,
                   timestamp,
                   index % 3 + 1,
                   index % 3 + 1,
                   "/run/smart-guard/latest.jpg",
                   error,
                   sizeof(error)) == 0);
    }

    assert(sg_history_record(
               database,
               "2026-07-27T20:00:07+04:00",
               1,
               1,
               "/run/smart-guard/latest.jpg",
               error,
               sizeof(error)) == 1);
    assert(sg_history_build_json(
               database, 5, json, sizeof(json), error, sizeof(error)) == 0);
    assert(strstr(json, "\"total_detection_events\":7") != NULL);
    assert(strstr(json, "\"stored_records\":5") != NULL);
    assert(strstr(json, "\"capacity\":5") != NULL);
    assert(strstr(json, "\"sequence\":7") != NULL);
    assert(strstr(json, "\"sequence\":1") == NULL);

    sg_history_close(database);
    unlink(path);
    rmdir(directory);
    puts("SQLite circular history tests passed");
    return 0;
}
