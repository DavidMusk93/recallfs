#define _POSIX_C_SOURCE 200809L

#include "sqlite3.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    MODE_UNSAFE,
    MODE_SERIALIZED,
    MODE_ROLLBACK,
} run_mode;

static sqlite3 *writer_db;
static atomic_int writer_stop;
static atomic_long committed_count;
static atomic_long writer_error_count;
static pthread_mutex_t operation_gate = PTHREAD_MUTEX_INITIALIZER;
static run_mode selected_mode;

static void fail_sqlite(sqlite3 *db, const char *operation, int rc) {
    fprintf(stderr, "%s failed: rc=%d message=%s\n", operation, rc,
            db == NULL ? "no database handle" : sqlite3_errmsg(db));
    exit(2);
}

static void exec_checked(sqlite3 *db, const char *sql, const char *operation) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "%s failed: rc=%d message=%s\n", operation, rc, message == NULL ? sqlite3_errmsg(db) : message);
        sqlite3_free(message);
        exit(2);
    }
}

static int64_t scalar_i64(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    int64_t value = -1;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return value;
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fail_sqlite(db, "sqlite3_open", rc);
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

static void gate_lock(void) {
    if (selected_mode == MODE_SERIALIZED) {
        int rc = pthread_mutex_lock(&operation_gate);
        if (rc != 0) {
            fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(rc));
            exit(2);
        }
    }
}

static void gate_unlock(void) {
    if (selected_mode == MODE_SERIALIZED) {
        int rc = pthread_mutex_unlock(&operation_gate);
        if (rc != 0) {
            fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(rc));
            exit(2);
        }
    }
}

static void *writer_main(void *unused) {
    (void)unused;
    sqlite3_stmt *insert = NULL;
    int rc = sqlite3_prepare_v2(writer_db, "INSERT INTO canary VALUES(?1)", -1, &insert, NULL);
    if (rc != SQLITE_OK) {
        atomic_fetch_add_explicit(&writer_error_count, 1, memory_order_relaxed);
        return NULL;
    }

    while (!atomic_load_explicit(&writer_stop, memory_order_acquire)) {
        int64_t next = atomic_load_explicit(&committed_count, memory_order_relaxed) + 1;

        gate_lock();
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        rc = sqlite3_bind_int64(insert, 1, next);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(insert);
        }
        if (rc == SQLITE_DONE) {
            atomic_fetch_add_explicit(&committed_count, 1, memory_order_release);
        } else if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            atomic_fetch_add_explicit(&writer_error_count, 1, memory_order_relaxed);
        }
        gate_unlock();
        if (selected_mode == MODE_SERIALIZED) {
            sched_yield();
        }
    }

    sqlite3_finalize(insert);
    return NULL;
}

static long parse_positive_long(const char *name, const char *text, long fallback) {
    if (text == NULL || *text == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static run_mode parse_mode(const char *text) {
    if (text == NULL || strcmp(text, "unsafe") == 0) {
        return MODE_UNSAFE;
    }
    if (strcmp(text, "serialized") == 0) {
        return MODE_SERIALIZED;
    }
    if (strcmp(text, "rollback") == 0) {
        return MODE_ROLLBACK;
    }
    fprintf(stderr, "invalid WALRACE_MODE: %s\n", text);
    exit(2);
}

static const char *mode_name(run_mode mode) {
    switch (mode) {
    case MODE_UNSAFE:
        return "unsafe";
    case MODE_SERIALIZED:
        return "serialized";
    case MODE_ROLLBACK:
        return "rollback";
    }
    return "unknown";
}

static void remove_database_files(const char *path) {
    size_t length = strlen(path);
    char *sidecar = malloc(length + 5);
    if (sidecar == NULL) {
        fputs("failed to allocate sidecar path\n", stderr);
        exit(2);
    }

    unlink(path);
    snprintf(sidecar, length + 5, "%s-wal", path);
    unlink(sidecar);
    snprintf(sidecar, length + 5, "%s-shm", path);
    unlink(sidecar);
    free(sidecar);
}

static const char *require_argument(int argc, char **argv, int *index) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", argv[*index]);
        exit(2);
    }
    *index += 1;
    return argv[*index];
}

int main(int argc, char **argv) {
    const char *path = getenv("WALRACE_DB");
    if (path == NULL || *path == '\0') {
        path = "race.db";
    }
    const char *mode_text = getenv("WALRACE_MODE");
    const char *attempts_text = getenv("WALRACE_ATTEMPTS");
    const char *rows_text = getenv("WALRACE_ROWS");

    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--db") == 0) {
            path = require_argument(argc, argv, &index);
        } else if (strcmp(argv[index], "--mode") == 0) {
            mode_text = require_argument(argc, argv, &index);
        } else if (strcmp(argv[index], "--attempts") == 0) {
            attempts_text = require_argument(argc, argv, &index);
        } else if (strcmp(argv[index], "--rows") == 0) {
            rows_text = require_argument(argc, argv, &index);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }

    selected_mode = parse_mode(mode_text);
    long attempts = parse_positive_long("attempts", attempts_text, 200);
    long rows = parse_positive_long("rows", rows_text, 65536);

    remove_database_files(path);
    sqlite3 *checkpointer = open_db(path);
    const char *journal = selected_mode == MODE_ROLLBACK ? "delete" : "wal";
    char *setup = sqlite3_mprintf("PRAGMA journal_mode=%s;"
                                  "PRAGMA mmap_size=1073741824;"
                                  "CREATE TABLE t1(a INTEGER PRIMARY KEY, b);"
                                  "CREATE TABLE canary(a INTEGER PRIMARY KEY);"
                                  "WITH s(i) AS ("
                                  "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%ld"
                                  ") INSERT INTO t1 SELECT NULL, randomblob(3900) FROM s;",
                                  journal, rows);
    if (setup == NULL) {
        fputs("sqlite3_mprintf failed\n", stderr);
        return 2;
    }
    exec_checked(checkpointer, setup, "database setup");
    sqlite3_free(setup);
    if (selected_mode != MODE_ROLLBACK) {
        exec_checked(checkpointer, "PRAGMA wal_checkpoint(TRUNCATE)", "initial checkpoint");
    }

    sqlite3 *helper = open_db(path);
    writer_db = open_db(path);
    long attempts_completed = 0;

    for (long attempt = 0; attempt < attempts; ++attempt) {
        pthread_t writer;

        if (scalar_i64(checkpointer, "SELECT count(*) FROM t1 WHERE b IS NOT NULL") != rows) {
            fputs("failed to read the large table\n", stderr);
            return 2;
        }

        exec_checked(helper, "UPDATE t1 SET b=randomblob(3900) WHERE a<=20", "prepare checkpoint state");

        if (selected_mode != MODE_ROLLBACK) {
            for (int pass = 0; pass < 50; ++pass) {
                int log_frames = 0;
                int checkpointed_frames = 0;
                int rc = sqlite3_wal_checkpoint_v2(helper, "main", SQLITE_CHECKPOINT_PASSIVE, &log_frames,
                                                   &checkpointed_frames);
                if (rc != SQLITE_OK) {
                    fail_sqlite(helper, "precondition checkpoint", rc);
                }
                if (checkpointed_frames >= log_frames) {
                    break;
                }
            }
        }

        atomic_store_explicit(&writer_stop, 0, memory_order_release);
        int64_t committed_before = atomic_load_explicit(&committed_count, memory_order_acquire);
        long errors_before = atomic_load_explicit(&writer_error_count, memory_order_relaxed);
        int thread_rc = pthread_create(&writer, NULL, writer_main, NULL);
        if (thread_rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(thread_rc));
            return 2;
        }

        if (selected_mode != MODE_UNSAFE) {
            while (atomic_load_explicit(&committed_count, memory_order_acquire) == committed_before &&
                   atomic_load_explicit(&writer_error_count, memory_order_relaxed) == errors_before) {
                sched_yield();
            }
        }

        gate_lock();
        int checkpoint_rc = sqlite3_wal_checkpoint_v2(checkpointer, "main", SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        gate_unlock();
        if (checkpoint_rc != SQLITE_OK && checkpoint_rc != SQLITE_BUSY) {
            fail_sqlite(checkpointer, "racing checkpoint", checkpoint_rc);
        }

        atomic_store_explicit(&writer_stop, 1, memory_order_release);
        thread_rc = pthread_join(writer, NULL);
        if (thread_rc != 0) {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(thread_rc));
            return 2;
        }
        attempts_completed = attempt + 1;

        int64_t visible = scalar_i64(helper, "SELECT count(*) FROM canary");
        int64_t committed = atomic_load_explicit(&committed_count, memory_order_acquire);
        if (visible < 0 || visible < committed) {
            break;
        }
    }

    if (selected_mode != MODE_ROLLBACK) {
        int rc = sqlite3_wal_checkpoint_v2(helper, "main", SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        if (rc != SQLITE_OK) {
            fail_sqlite(helper, "final truncate checkpoint", rc);
        }
    }

    int64_t committed = atomic_load_explicit(&committed_count, memory_order_acquire);
    int64_t recovered = scalar_i64(helper, "SELECT count(*) FROM canary");
    int64_t integrity_errors = scalar_i64(helper, "SELECT count(*) FROM pragma_integrity_check "
                                                  "WHERE integrity_check <> 'ok'");
    int64_t lost = recovered < 0 ? committed : committed - recovered;

    printf("sqlite_version=%s\n", sqlite3_libversion());
    printf("mode=%s\n", mode_name(selected_mode));
    printf("attempts=%ld\n", attempts_completed);
    printf("committed=%lld\n", (long long)committed);
    printf("recovered=%lld\n", (long long)recovered);
    printf("lost=%lld\n", (long long)lost);
    printf("integrity=%s\n", integrity_errors == 0 ? "ok" : "failed-or-unreadable");
    printf("writer_errors=%ld\n", atomic_load_explicit(&writer_error_count, memory_order_relaxed));
    printf("outcome=%s\n", lost > 0 ? "loss-detected" : "no-loss-observed");

    sqlite3_close(writer_db);
    sqlite3_close(helper);
    sqlite3_close(checkpointer);
    return lost > 0 || integrity_errors != 0 ? 1 : 0;
}
