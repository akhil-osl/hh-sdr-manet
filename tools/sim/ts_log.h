/*
 * Tiny logging helper shared by telemetry_serve.c and telemetry_server.c —
 * DEVELOPMENT/TEST TOOLING ONLY, same status as both those files.
 *
 * Writes every message to stderr (as before) and, if a log file was opened
 * via hh_ts_log_open(), to that file too. One process-wide destination is
 * enough here: this tool runs as a single instance (see telemetry_server.c's
 * g_server singleton note).
 */
#ifndef HH_TS_LOG_H
#define HH_TS_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *hh_ts_log_file = NULL;

static inline int hh_ts_log_open(const char *path)
{
    hh_ts_log_file = fopen(path, "a");
    return hh_ts_log_file != NULL;
}

static inline void hh_ts_log_close(void)
{
    if (hh_ts_log_file) { fclose(hh_ts_log_file); hh_ts_log_file = NULL; }
}

/* printf-style; appends a timestamp when writing to the file (stderr stays
 * as terse as before, since a terminal already shows wall-clock context). */
static inline void hh_ts_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (hh_ts_log_file) {
        /* Single-threaded tool (see module comment), so the classic
         * localtime() non-reentrancy concern does not apply here. */
        char ts[32];
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        if (tm_now) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm_now);
        else strncpy(ts, "?", sizeof ts);

        fprintf(hh_ts_log_file, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(hh_ts_log_file, fmt, ap);
        va_end(ap);
        fflush(hh_ts_log_file);
    }
}

#endif /* HH_TS_LOG_H */
