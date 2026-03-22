// 2026-03-22 v0.6.1
// terminal_view.h — GtkDrawingArea with libttcore rendering via Cairo+Pango.
// Clear Screen (ESC[2J + ESC[H via ttcore_parse_data).
// Phase G10b: mouse selection + Copy to clipboard.
// Phase G5: cursor styles (block/underline, blink, DECSCUSR), focus tracking.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_TERMINAL_VIEW_H_
#define GUI_TERMINAL_VIEW_H_

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create a terminal view backed by libttcore.
// Canvas starts empty — data is fed via terminal_view_feed_data().
// The returned widget is owned by the caller (GTK ref-counting).
GtkWidget *terminal_view_create(void);

// Destroy the libttcore instance (called on application shutdown).
void terminal_view_destroy(void);

// Feed raw bytes from the serial connection into the terminal engine.
// Calls ttcore_parse_data() and queues a redraw.
void terminal_view_feed_data(const uint8_t *data, int len);

// Force a full redraw (e.g. after color change).
void terminal_view_queue_draw(void);

// Re-apply font and colors from view_settings.
// Recalculates cell size, calls ttcore_resize() if dimensions changed.
void terminal_view_apply_settings(void);

// Grab keyboard focus to the terminal drawing area.
void terminal_view_grab_focus(void);

// Suppress ttcore_resize() calls during GtkRevealer animation.
void terminal_view_suppress_resize(bool suppress);

// Apply the pending resize that was deferred while suppressed.
void terminal_view_apply_pending_resize(void);

// Set cursor style programmatically (from View menu or settings).
// shape: VTPARSER_CURSOR_BLOCK (0), UNDERLINE_STEADY (4), etc.
// blink: true = blink enabled.
// Note: DECSCUSR from the remote host overrides this via callback.
void terminal_view_set_cursor_style(int shape, bool blink);

// Set backspace key behavior.
// use_del=true: BackSpace sends DEL (0x7F).
// use_del=false: BackSpace sends BS (0x08).  Default: false.
void terminal_view_set_backspace_del(bool use_del);

// Copy selected text to clipboard.  Returns true if text was copied.
bool terminal_view_copy_selection(void);

// Returns true if a text selection is active.
bool terminal_view_has_selection(void);

// Clear screen: feeds ESC[2J ESC[H into the terminal engine and redraws.
void terminal_view_clear_screen(void);

#ifdef __cplusplus
}
#endif

#endif  // GUI_TERMINAL_VIEW_H_
