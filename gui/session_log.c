// 2026-03-22 v0.1.0
// session_log.c — Session log: raw RX/TX bytes to plain text file.
// Phase G11b: timestamps, TX logging, auto-start, append/overwrite.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "session_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

typedef struct {
    FILE *fp;            // Log file handle (NULL = inactive)
    bool log_tx;         // Log TX bytes?
    bool timestamps;     // Prepend timestamp per line?
    bool at_line_start;  // Next byte starts a new line?
    char path[512];      // Active log file path
} SessionLogState;

static SessionLogState g_log;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Write timestamp prefix: [2026-03-22 14:30:05.123] or [... RX]/[... TX]
static void write_timestamp(FILE *fp, bool log_tx, const char *direction) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int ms = (int)(ts.tv_nsec / 1000000);
    if (log_tx) {
        fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d.%03d %s] ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms, direction);
    } else {
        fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }
}

// Write data with optional timestamps.  direction is "RX" or "TX".
static void write_data(const uint8_t *data, size_t len,
                        const char *direction) {
    if (!g_log.fp || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        if (g_log.at_line_start && g_log.timestamps) {
            write_timestamp(g_log.fp, g_log.log_tx, direction);
            g_log.at_line_start = false;
        }
        fputc(data[i], g_log.fp);
        if (data[i] == '\n') {
            g_log.at_line_start = true;
        }
    }
    fflush(g_log.fp);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool session_log_start(const char *path, bool append,
                       bool log_tx, bool timestamps) {
    if (!path || path[0] == '\0') return false;

    // Stop any existing log first.
    session_log_stop();

    FILE *fp = fopen(path, append ? "a" : "w");
    if (!fp) return false;

    g_log.fp = fp;
    g_log.log_tx = log_tx;
    g_log.timestamps = timestamps;
    g_log.at_line_start = true;
    snprintf(g_log.path, sizeof(g_log.path), "%s", path);

    return true;
}

void session_log_stop(void) {
    if (g_log.fp) {
        fflush(g_log.fp);
        fclose(g_log.fp);
    }
    memset(&g_log, 0, sizeof(g_log));
}

bool session_log_is_active(void) {
    return g_log.fp != NULL;
}

void session_log_write_rx(const uint8_t *data, size_t len) {
    if (!g_log.fp) return;
    write_data(data, len, "RX");
}

void session_log_write_tx(const uint8_t *data, size_t len) {
    if (!g_log.fp || !g_log.log_tx) return;
    write_data(data, len, "TX");
}
