// 2026-03-21 v0.2.0
// xmodem_gui.c — XMODEM file transfer GUI integration (GLib timer + RX ring).
// Phase G10: XMODEM Send + Receive via ttcore_transfer + ttcore_io mode switch.
// Manages RX ring buffer, GLib one-shot timeout, progress reporting.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "xmodem_gui.h"
#include "serial_conn.h"
#include "text_sender.h"
#include "status_bar.h"

#include "ttcore_transfer.h"
#include "ttcore_io.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// RX Ring Buffer (local to this module — SoC)
// ---------------------------------------------------------------------------

#define RX_RING_SIZE 4096

typedef struct {
    uint8_t buf[RX_RING_SIZE];
    size_t head;
    size_t tail;
    size_t count;
} RxRing;

static void rx_ring_clear(RxRing *r) {
    r->head = r->tail = r->count = 0;
}

static void rx_ring_push(RxRing *r, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (r->count < RX_RING_SIZE) {
            r->buf[r->head] = data[i];
            r->head = (r->head + 1) % RX_RING_SIZE;
            r->count++;
        }
    }
}

static int rx_ring_pop(RxRing *r, uint8_t *b) {
    if (r->count == 0) return 0;
    *b = r->buf[r->tail];
    r->tail = (r->tail + 1) % RX_RING_SIZE;
    r->count--;
    return 1;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TtcoreTransfer *g_xfer = NULL;
static RxRing g_rx_ring;
static guint g_timeout_id = 0;
static FILE *g_file = NULL;
static bool g_is_recv = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void xmodem_gui_cleanup(TtcoreXferResult result);

// ---------------------------------------------------------------------------
// ttcore_transfer callbacks
// ---------------------------------------------------------------------------

static int xfer_write_bytes(void *user, const uint8_t *buf, size_t len) {
    (void)user;
    return serial_conn_write(buf, (int)len);
}

static int xfer_read_byte(void *user, uint8_t *b) {
    (void)user;
    return rx_ring_pop(&g_rx_ring, b);
}

static void xfer_flush_rx(void *user) {
    (void)user;
    rx_ring_clear(&g_rx_ring);
}

static int xfer_file_open_read(void *user, const char *path) {
    (void)user;
    g_file = fopen(path, "rb");
    return g_file ? 1 : 0;
}

static size_t xfer_file_read(void *user, uint8_t *buf, size_t len) {
    (void)user;
    if (!g_file) return 0;
    return fread(buf, 1, len, g_file);
}

static void xfer_file_close(void *user) {
    (void)user;
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
}

static int xfer_file_open_write(void *user, const char *path) {
    (void)user;
    g_file = fopen(path, "wb");
    return g_file ? 1 : 0;
}

static size_t xfer_file_write(void *user, const uint8_t *buf, size_t len) {
    (void)user;
    if (!g_file) return 0;
    return fwrite(buf, 1, len, g_file);
}

static long xfer_file_size(void *user, const char *path) {
    (void)user;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

// ---------------------------------------------------------------------------
// GLib timeout integration for set_timeout callback
// ---------------------------------------------------------------------------

static gboolean on_xfer_timeout_tick(gpointer user_data) {
    (void)user_data;
    g_timeout_id = 0;
    if (!g_xfer) return G_SOURCE_REMOVE;

    ttcore_xfer_timeout(g_xfer);
    TtcoreXferResult r = ttcore_xfer_parse(g_xfer);
    if (r == TTCORE_XFER_DONE || r == TTCORE_XFER_CANCELED || r < 0) {
        xmodem_gui_cleanup(r);
    }
    return G_SOURCE_REMOVE;
}

static void xfer_set_timeout(void *user, int ms) {
    (void)user;
    if (g_timeout_id) {
        g_source_remove(g_timeout_id);
        g_timeout_id = 0;
    }
    if (ms > 0) {
        g_timeout_id = g_timeout_add((guint)ms, on_xfer_timeout_tick, NULL);
    }
}

// ---------------------------------------------------------------------------
// Progress + done callbacks
// ---------------------------------------------------------------------------

static void xfer_on_progress(void *user, const TtcoreXferStatus *status) {
    (void)user;
    char msg[128];
    if (g_is_recv) {
        snprintf(msg, sizeof(msg), "XMODEM: block %ld, %ld bytes received",
                 status->packet_num, status->bytes_transferred);
    } else if (status->file_size > 0) {
        int pct = (int)(status->bytes_transferred * 100 / status->file_size);
        snprintf(msg, sizeof(msg), "XMODEM: block %ld, %ld/%ld bytes (%d%%)",
                 status->packet_num, status->bytes_transferred,
                 status->file_size, pct);
    } else {
        snprintf(msg, sizeof(msg), "XMODEM: block %ld, %ld bytes",
                 status->packet_num, status->bytes_transferred);
    }
    status_bar_set_progress(msg);
}

static void xfer_on_done(void *user, TtcoreXferResult result) {
    (void)user;
    xmodem_gui_cleanup(result);
}

// ---------------------------------------------------------------------------
// RX data callback (registered with serial_conn)
// ---------------------------------------------------------------------------

static void on_xfer_rx_data(const uint8_t *data, size_t len, void *ud) {
    (void)ud;
    rx_ring_push(&g_rx_ring, data, len);

    if (!g_xfer) return;
    TtcoreXferResult r = ttcore_xfer_parse(g_xfer);
    if (r == TTCORE_XFER_DONE || r == TTCORE_XFER_CANCELED || r < 0) {
        xmodem_gui_cleanup(r);
    }
}

// ---------------------------------------------------------------------------
// Cleanup helper
// ---------------------------------------------------------------------------

static void xmodem_gui_cleanup(TtcoreXferResult result) {
    // Cancel pending timeout
    if (g_timeout_id) {
        g_source_remove(g_timeout_id);
        g_timeout_id = 0;
    }

    // Status message
    if (result == TTCORE_XFER_DONE) {
        const TtcoreXferStatus *st = g_xfer ? ttcore_xfer_status(g_xfer)
                                             : NULL;
        char msg[128];
        snprintf(msg, sizeof(msg), "XMODEM: complete, %ld bytes%s",
                 st ? st->bytes_transferred : 0L,
                 g_is_recv ? " received" : "");
        status_bar_set_progress(msg);
    } else if (result == TTCORE_XFER_CANCELED) {
        status_bar_set_progress("XMODEM: canceled");
    } else {
        const char *reason = "unknown error";
        switch (result) {
            case TTCORE_XFER_ERR_PROTO:   reason = "protocol error";  break;
            case TTCORE_XFER_ERR_IO:      reason = "I/O error";       break;
            case TTCORE_XFER_ERR_TIMEOUT: reason = "timeout";         break;
            case TTCORE_XFER_ERR_NOMEM:   reason = "out of memory";   break;
            default: break;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "XMODEM: failed — %s", reason);
        status_bar_set_error(msg);
        status_bar_set_progress(NULL);
    }

    // Destroy transfer
    if (g_xfer) {
        ttcore_xfer_destroy(g_xfer);
        g_xfer = NULL;
    }

    // Close file if still open
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }

    // Switch back to terminal mode
    serial_conn_set_transfer_mode(false);
    serial_conn_set_xfer_callback(NULL, NULL);

    rx_ring_clear(&g_rx_ring);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool xmodem_gui_send_start(const char *filepath, TtcoreXmodemOpt opt) {
    if (!filepath) return false;
    if (g_xfer) return false;
    if (!serial_conn_is_open()) {
        status_bar_set_error("XMODEM: not connected");
        return false;
    }
    if (text_sender_is_active()) {
        status_bar_set_error("XMODEM: a text send is in progress");
        return false;
    }

    g_is_recv = false;

    // Switch to transfer mode + register RX callback
    serial_conn_set_xfer_callback(on_xfer_rx_data, NULL);
    serial_conn_set_transfer_mode(true);

    // Flush stale RX bytes (HW-07 experience: flush after mode switch)
    rx_ring_clear(&g_rx_ring);

    // Configure transfer
    TtcoreXferConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proto = TTCORE_XFER_PROTO_XMODEM;
    cfg.dir = TTCORE_XFER_SEND;
    cfg.opt = (int)opt;
    strncpy(cfg.filepath, filepath, sizeof(cfg.filepath) - 1);

    TtcoreXferCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.write_bytes = xfer_write_bytes;
    cbs.read_byte = xfer_read_byte;
    cbs.flush_rx = xfer_flush_rx;
    cbs.file_open_read = xfer_file_open_read;
    cbs.file_open_write = xfer_file_open_write;
    cbs.file_read = xfer_file_read;
    cbs.file_write = xfer_file_write;
    cbs.file_close = xfer_file_close;
    cbs.file_size = xfer_file_size;
    cbs.set_timeout = xfer_set_timeout;
    cbs.on_progress = xfer_on_progress;
    cbs.on_done = xfer_on_done;
    cbs.user = NULL;

    g_xfer = ttcore_xfer_create(&cfg, &cbs);
    if (!g_xfer) {
        status_bar_set_error("XMODEM: failed to create transfer");
        serial_conn_set_transfer_mode(false);
        serial_conn_set_xfer_callback(NULL, NULL);
        return false;
    }

    status_bar_set_progress("XMODEM: waiting for receiver...");
    return true;
}

bool xmodem_gui_recv_start(const char *filepath, TtcoreXmodemOpt opt) {
    if (!filepath) return false;
    if (g_xfer) return false;
    if (!serial_conn_is_open()) {
        status_bar_set_error("XMODEM: not connected");
        return false;
    }
    if (text_sender_is_active()) {
        status_bar_set_error("XMODEM: a text send is in progress");
        return false;
    }

    g_is_recv = true;

    // Switch to transfer mode + register RX callback
    serial_conn_set_xfer_callback(on_xfer_rx_data, NULL);
    serial_conn_set_transfer_mode(true);

    // Flush stale RX bytes (HW-07/HW-11 proven)
    rx_ring_clear(&g_rx_ring);

    // Configure transfer
    TtcoreXferConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proto = TTCORE_XFER_PROTO_XMODEM;
    cfg.dir = TTCORE_XFER_RECV;
    cfg.opt = (int)opt;
    strncpy(cfg.filepath, filepath, sizeof(cfg.filepath) - 1);

    TtcoreXferCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.write_bytes = xfer_write_bytes;
    cbs.read_byte = xfer_read_byte;
    cbs.flush_rx = xfer_flush_rx;
    cbs.file_open_read = xfer_file_open_read;
    cbs.file_open_write = xfer_file_open_write;
    cbs.file_read = xfer_file_read;
    cbs.file_write = xfer_file_write;
    cbs.file_close = xfer_file_close;
    cbs.file_size = xfer_file_size;
    cbs.set_timeout = xfer_set_timeout;
    cbs.on_progress = xfer_on_progress;
    cbs.on_done = xfer_on_done;
    cbs.user = NULL;

    g_xfer = ttcore_xfer_create(&cfg, &cbs);
    if (!g_xfer) {
        status_bar_set_error("XMODEM: failed to create transfer");
        serial_conn_set_transfer_mode(false);
        serial_conn_set_xfer_callback(NULL, NULL);
        return false;
    }

    status_bar_set_progress("XMODEM: waiting for sender...");
    return true;
}

void xmodem_gui_cancel(void) {
    if (!g_xfer) return;
    ttcore_xfer_cancel(g_xfer);
    xmodem_gui_cleanup(TTCORE_XFER_CANCELED);
}

bool xmodem_gui_is_active(void) {
    return g_xfer != NULL;
}
