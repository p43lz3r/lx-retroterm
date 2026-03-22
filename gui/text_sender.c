// 2026-03-20 v0.1.0
// text_sender.c — Line-by-line text send engine with GLib timer.
// Phase G7: Send Text / Send Text File.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "text_sender.h"
#include "serial_conn.h"
#include "status_bar.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static char *g_text_buf = NULL;
static int g_total_lines = 0;
static int g_current_line = 0;
static TextSenderEol g_eol = TEXT_SENDER_EOL_CR;
static guint g_timer_id = 0;

// Pointer to current position within g_text_buf.
static const char *g_pos = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int count_lines(const char *text) {
    int count = 0;
    const char *p = text;
    while (*p) {
        if (*p == '\n') count++;
        p++;
    }
    // Count the last line if it doesn't end with \n.
    if (p > text && *(p - 1) != '\n') count++;
    return count > 0 ? count : 0;
}

static const uint8_t *eol_bytes(TextSenderEol eol, int *len) {
    static const uint8_t cr[] = {'\r'};
    static const uint8_t lf[] = {'\n'};
    static const uint8_t crlf[] = {'\r', '\n'};
    switch (eol) {
        case TEXT_SENDER_EOL_LF:   *len = 1; return lf;
        case TEXT_SENDER_EOL_CRLF: *len = 2; return crlf;
        default:                   *len = 1; return cr;
    }
}

// Send one line from g_pos.  Advances g_pos past the line.
// Returns true if a line was sent, false if no more data.
static bool send_one_line(void) {
    if (!g_pos || *g_pos == '\0') return false;
    if (!serial_conn_is_open()) return false;

    const char *end = strchr(g_pos, '\n');
    int line_len;
    if (end) {
        line_len = (int)(end - g_pos);
    } else {
        line_len = (int)strlen(g_pos);
    }

    if (line_len > 0) {
        serial_conn_write((const uint8_t *)g_pos, line_len);
    }

    int eol_len;
    const uint8_t *eol = eol_bytes(g_eol, &eol_len);
    serial_conn_write(eol, eol_len);

    g_pos = end ? end + 1 : g_pos + line_len;
    return true;
}

static void cleanup(void) {
    free(g_text_buf);
    g_text_buf = NULL;
    g_pos = NULL;
    g_timer_id = 0;
    g_total_lines = 0;
    g_current_line = 0;
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

static gboolean on_send_timer(gpointer user_data) {
    (void)user_data;

    if (!serial_conn_is_open()) {
        status_bar_set_progress("Send aborted: disconnected");
        cleanup();
        return G_SOURCE_REMOVE;
    }

    if (!send_one_line()) {
        status_bar_set_progress("Send complete");
        cleanup();
        return G_SOURCE_REMOVE;
    }

    g_current_line++;
    char msg[64];
    snprintf(msg, sizeof(msg), "Sending line %d/%d...",
             g_current_line, g_total_lines);
    status_bar_set_progress(msg);

    if (g_current_line >= g_total_lines) {
        status_bar_set_progress("Send complete");
        cleanup();
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool text_sender_start(const char *text, TextSenderEol eol, int delay_ms) {
    if (g_timer_id != 0 || g_text_buf != NULL) return false;
    if (!text || *text == '\0') return false;
    if (!serial_conn_is_open()) return false;

    g_text_buf = strdup(text);
    if (!g_text_buf) return false;

    g_eol = eol;
    g_total_lines = count_lines(g_text_buf);
    g_current_line = 0;
    g_pos = g_text_buf;

    if (delay_ms <= 0) {
        // Send all lines at once, checking connection before each write.
        while (*g_pos != '\0') {
            if (!serial_conn_is_open()) {
                status_bar_set_progress("Send aborted: disconnected");
                cleanup();
                return true;
            }
            send_one_line();
            g_current_line++;
        }
        status_bar_set_progress("Send complete");
        cleanup();
        return true;
    }

    // Send first line immediately, then arm timer for the rest.
    send_one_line();
    g_current_line++;

    if (g_current_line >= g_total_lines) {
        status_bar_set_progress("Send complete");
        cleanup();
        return true;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Sending line %d/%d...",
             g_current_line, g_total_lines);
    status_bar_set_progress(msg);

    g_timer_id = g_timeout_add((guint)delay_ms, on_send_timer, NULL);
    return true;
}

void text_sender_cancel(void) {
    if (g_timer_id) {
        g_source_remove(g_timer_id);
    }
    if (g_text_buf) {
        status_bar_set_progress(NULL);
    }
    cleanup();
}

bool text_sender_is_active(void) {
    return g_text_buf != NULL;
}
