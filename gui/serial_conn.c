// 2026-03-22 v0.4.1
// serial_conn.c — Serial connection module (single-threaded, poll-driven).
// Phase G11b: session_log_write_rx/tx integration; TX log suppressed in transfer mode.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "serial_conn.h"
#include "config.h"
#include "session_log.h"
#include "text_sender.h"
#include "xmodem_gui.h"
#include "terminal_view.h"
#include "status_bar.h"

#include "ttcore_io.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TtcoreIo *g_io = NULL;
static guint g_poll_timer_id = 0;
static guint g_throughput_timer_id = 0;
static uint32_t g_rx_bytes = 0;
static uint32_t g_throughput = 0;
static bool g_local_echo = false;
static bool g_transfer_mode = false;
static serial_conn_xfer_fn g_xfer_fn = NULL;
static void *g_xfer_ud = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

// Defined in app_window.c
void app_window_notify_connected(bool connected);

// ---------------------------------------------------------------------------
// Callbacks from ttcore_io
// ---------------------------------------------------------------------------

static void on_rx_data(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    g_rx_bytes += (uint32_t)len;
    session_log_write_rx(data, len);
    terminal_view_feed_data(data, (int)len);
}

static void on_xfer_data(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    if (g_xfer_fn) {
        g_xfer_fn(data, len, g_xfer_ud);
    }
}

static void on_open(void *ud) {
    (void)ud;
    app_window_notify_connected(true);
}

static void on_close(void *ud) {
    (void)ud;
    xmodem_gui_cancel();
    text_sender_cancel();
    app_window_notify_connected(false);
}

static void on_error(int code, const char *msg, void *ud) {
    (void)ud;
    (void)code;
    status_bar_set_error(msg);
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

static gboolean on_poll_tick(gpointer user_data) {
    (void)user_data;
    if (g_io && ttcore_io_is_open(g_io)) {
        ttcore_io_poll(g_io, 0);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_throughput_tick(gpointer user_data) {
    (void)user_data;
    g_throughput = g_rx_bytes;
    g_rx_bytes = 0;
    status_bar_update_throughput(g_throughput);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int serial_conn_open(const char *device, uint32_t baud,
                     uint8_t data_bits, uint8_t stop_bits,
                     uint8_t parity, uint8_t flow) {
    if (g_io) serial_conn_close();

    g_io = ttcore_io_create();
    if (!g_io) return -1;

    TtcoreIoConfig cfg = {0};
    cfg.device = device;
    cfg.baud = baud;
    cfg.data_bits = data_bits;
    cfg.stop_bits = stop_bits;
    cfg.parity = parity;
    cfg.flow = flow;

    int rc = ttcore_io_configure(g_io, &cfg);
    if (rc != TTCORE_IO_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Configure failed: %d", rc);
        status_bar_set_error(msg);
        ttcore_io_destroy(g_io);
        g_io = NULL;
        return rc;
    }

    TtcoreIoCallbacks cbs = {0};
    cbs.on_rx_data = on_rx_data;
    cbs.on_xfer_data = on_xfer_data;
    cbs.on_open = on_open;
    cbs.on_close = on_close;
    cbs.on_error = on_error;
    ttcore_io_set_callbacks(g_io, &cbs);

    rc = ttcore_io_open(g_io);
    if (rc != TTCORE_IO_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Open failed: %s (%d)", device, rc);
        status_bar_set_error(msg);
        ttcore_io_destroy(g_io);
        g_io = NULL;
        return rc;
    }

    // Start poll timer (20ms)
    g_rx_bytes = 0;
    g_throughput = 0;
    g_poll_timer_id = g_timeout_add(20, on_poll_tick, NULL);
    g_throughput_timer_id = g_timeout_add(1000, on_throughput_tick, NULL);

    // Auto-start session log if configured.
    if (!session_log_is_active() &&
        config_get_bool("Log", "auto_start", false)) {
        const char *auto_path = config_get_string("Log", "auto_path", "");
        if (auto_path && auto_path[0] != '\0') {
            bool log_tx = config_get_bool("Log", "log_tx", false);
            bool ts = config_get_bool("Log", "timestamps", true);
            bool append = config_get_bool("Log", "append", false);
            if (!session_log_start(auto_path, append, log_tx, ts)) {
#ifdef TTCORE_DEBUG
                fprintf(stderr, "session_log: auto-start skipped — "
                        "cannot open '%s'\n", auto_path);
#endif
            }
        } else {
#ifdef TTCORE_DEBUG
            fprintf(stderr, "session_log: auto-start skipped — "
                    "auto_path is empty\n");
#endif
        }
    }

    return 0;
}

void serial_conn_close(void) {
    if (g_poll_timer_id) {
        g_source_remove(g_poll_timer_id);
        g_poll_timer_id = 0;
    }
    if (g_throughput_timer_id) {
        g_source_remove(g_throughput_timer_id);
        g_throughput_timer_id = 0;
    }
    if (g_io) {
        if (ttcore_io_is_open(g_io)) {
            ttcore_io_close(g_io);
        }
        ttcore_io_destroy(g_io);
        g_io = NULL;
    }
    g_throughput = 0;
    g_rx_bytes = 0;
    status_bar_update_throughput(0);
}

bool serial_conn_is_open(void) {
    return g_io && ttcore_io_is_open(g_io);
}

int serial_conn_write(const uint8_t *data, int len) {
    if (!g_io || !ttcore_io_is_open(g_io)) return -1;
    int rc = ttcore_io_write(g_io, data, (size_t)len);
    if (rc >= 0) {
        if (!g_transfer_mode) {
            session_log_write_tx(data, (size_t)len);
        }
        if (g_local_echo) {
            terminal_view_feed_data(data, len);
        }
    }
    return rc;
}

int serial_conn_set_dtr(bool active) {
    if (!g_io || !ttcore_io_is_open(g_io)) return -1;
    return ttcore_io_set_dtr(g_io, active);
}

int serial_conn_set_rts(bool active) {
    if (!g_io || !ttcore_io_is_open(g_io)) return -1;
    return ttcore_io_set_rts(g_io, active);
}

uint32_t serial_conn_get_throughput(void) {
    return g_throughput;
}

void serial_conn_set_local_echo(bool enable) {
    g_local_echo = enable;
}

bool serial_conn_get_local_echo(void) {
    return g_local_echo;
}

void serial_conn_set_xfer_callback(serial_conn_xfer_fn fn, void *ud) {
    g_xfer_fn = fn;
    g_xfer_ud = ud;
}

void serial_conn_set_transfer_mode(bool transfer) {
    if (!g_io) return;
    g_transfer_mode = transfer;
    ttcore_io_set_mode(g_io, transfer ? TTCORE_IO_MODE_TRANSFER
                                      : TTCORE_IO_MODE_TERMINAL);
}
