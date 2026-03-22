// 2026-03-18 v0.2.0
// status_bar.h — Status bar with connection info labels.
// Phase G3: dynamic connection indicator, throughput, error display.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_STATUS_BAR_H_
#define GUI_STATUS_BAR_H_

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the status bar widget.
// The returned GtkBox is owned by the caller (GTK ref-counting).
GtkWidget *status_bar_create(void);

// Update the cursor position label.
void status_bar_update_cursor(int x, int y);

// Update the geometry (cols x lines) label.
void status_bar_update_geometry(int cols, int lines);

// Update the connection indicator (green/red dot + port/baud or "Disconnected").
void status_bar_set_connection(bool connected, const char *port, uint32_t baud);

// Display a transient error message.
void status_bar_set_error(const char *msg);

// Update the throughput counter (bytes/sec).
void status_bar_update_throughput(uint32_t bytes_per_sec);

// Show a progress message (e.g. "Sending line 3/10...").
// Pass NULL to clear the progress display.
void status_bar_set_progress(const char *msg);

#ifdef __cplusplus
}
#endif

#endif  // GUI_STATUS_BAR_H_
