// 2026-03-22 v0.1.0
// session_log.h — Session log: raw RX/TX bytes to plain text file.
// Phase G11b: timestamps, TX logging, auto-start, append/overwrite.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_SESSION_LOG_H_
#define GUI_SESSION_LOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open log file and start logging.
// append: true = fopen("a"), false = fopen("w").
// log_tx: true = also log transmitted bytes.
// timestamps: true = prepend [YYYY-MM-DD HH:MM:SS.mmm] per line.
// Returns true on success, false if path is NULL/empty or fopen fails.
bool session_log_start(const char *path, bool append,
                       bool log_tx, bool timestamps);

// Close log file.  Safe to call when inactive (no-op).
void session_log_stop(void);

// Returns true if logging is active.
bool session_log_is_active(void);

// Write received data to log.  No-op if inactive.
void session_log_write_rx(const uint8_t *data, size_t len);

// Write transmitted data to log.  No-op if inactive or log_tx is off.
void session_log_write_tx(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // GUI_SESSION_LOG_H_
