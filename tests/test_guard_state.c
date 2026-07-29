#define _POSIX_C_SOURCE 200809L

#include "guard_state.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/smart-guard-state-test.XXXXXX";
    char path[512];
    char error[512];
    bool enabled = true;

    assert(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/guard-mode", directory);

    assert(sg_guard_read(
               path, false, &enabled, error, sizeof(error)) == 0);
    assert(!enabled);

    assert(sg_guard_write_atomic(
               path, true, error, sizeof(error)) == 0);
    assert(sg_guard_read(
               path, false, &enabled, error, sizeof(error)) == 0);
    assert(enabled);

    assert(sg_guard_write_atomic(
               path, false, error, sizeof(error)) == 0);
    assert(sg_guard_read(
               path, true, &enabled, error, sizeof(error)) == 0);
    assert(!enabled);

    unlink(path);
    rmdir(directory);
    puts("guard state tests passed");
    return 0;
}
