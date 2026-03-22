// 2026-03-21 v0.1.0
// hex_upload.c — Intel HEX file upload via text_sender.
// Phase G8: reads .hex file, filters to valid ':' records, stops at EOF record,
// delegates line-by-line send to text_sender with EOL=CR.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "hex_upload.h"
#include "text_sender.h"
#include "xmodem_gui.h"
#include "status_bar.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool hex_upload_start(const char *filepath, int delay_ms) {
    if (!filepath) return false;
    if (text_sender_is_active() || xmodem_gui_is_active()) return false;

    char *raw = NULL;
    gsize raw_len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(filepath, &raw, &raw_len, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "HEX Upload: %s",
                 err ? err->message : "read error");
        status_bar_set_error(msg);
        if (err) g_error_free(err);
        return false;
    }

    // Build filtered text: only lines starting with ':', stop at EOF record.
    GString *filtered = g_string_new(NULL);
    int record_count = 0;

    char *line = raw;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t len;
        if (eol) {
            len = (size_t)(eol - line);
        } else {
            len = strlen(line);
        }

        // Strip trailing \r
        size_t trimmed = len;
        while (trimmed > 0 && line[trimmed - 1] == '\r') {
            trimmed--;
        }

        if (trimmed > 0 && line[0] == ':') {
            g_string_append_len(filtered, line, (gssize)trimmed);
            g_string_append_c(filtered, '\n');
            record_count++;

            // Check for EOF record (:00000001FF)
            if (trimmed >= 11 && strncasecmp(line, ":00000001FF", 11) == 0) {
                break;
            }
        }

        line = eol ? eol + 1 : line + len;
    }

    g_free(raw);

    if (record_count == 0) {
        status_bar_set_error("HEX Upload: no valid records found");
        g_string_free(filtered, TRUE);
        return false;
    }

    char *text = g_string_free(filtered, FALSE);
    bool ok = text_sender_start(text, TEXT_SENDER_EOL_CR, delay_ms);
    g_free(text);

    if (!ok) {
        status_bar_set_error("HEX Upload: failed to start send");
        return false;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "HEX Upload: %d records", record_count);
    status_bar_set_progress(msg);
    return true;
}

void hex_upload_cancel(void) {
    text_sender_cancel();
}
