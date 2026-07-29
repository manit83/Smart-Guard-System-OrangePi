#ifndef SQLITE3_H
#define SQLITE3_H

#include <stdarg.h>

typedef long long sqlite3_int64;
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101

#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_FULLMUTEX 0x00010000

#define SQLITE_TRANSIENT ((sqlite3_destructor_type) -1)

int sqlite3_open_v2(
    const char *,
    sqlite3 **,
    int,
    const char *);
int sqlite3_close(sqlite3 *);
int sqlite3_busy_timeout(sqlite3 *, int);
int sqlite3_exec(
    sqlite3 *,
    const char *,
    int (*)(void *, int, char **, char **),
    void *,
    char **);
void sqlite3_free(void *);
const char *sqlite3_errmsg(sqlite3 *);
const char *sqlite3_errstr(int);
int sqlite3_prepare_v2(
    sqlite3 *,
    const char *,
    int,
    sqlite3_stmt **,
    const char **);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_bind_int(sqlite3_stmt *, int, int);
int sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);
int sqlite3_bind_text(
    sqlite3_stmt *,
    int,
    const char *,
    int,
    sqlite3_destructor_type);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *, int);
int sqlite3_column_int(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);

#endif
