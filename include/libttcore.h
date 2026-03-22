// 2026-03-17 00:00 v0.2.3
// libttcore.h — Public API: headless VT terminal engine
// Integrates: ttcore_buffer, vtparse, vtcharset, vtmouse, vtcolor
// Win32-free, C99, callback-based, zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef LIBTTCORE_H_
#define LIBTTCORE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------
typedef struct ttcore_t ttcore_t;

// ---------------------------------------------------------------------------
// UI callbacks — implemented by the host application.
// Every pointer is optional: NULL means "ignore this event".
// ---------------------------------------------------------------------------
typedef struct {
    void *user_data;

    // Rendering
    void (*update_rect)(void *user_data, int x, int y, int w, int h);
    void (*scroll_screen)(void *user_data, int lines);
    void (*clear_screen)(void *user_data);
    void (*set_cursor_pos)(void *user_data, int x, int y);
    void (*set_cursor_style)(void *user_data, int shape, bool blink);

    // Window management
    void (*set_window_title)(void *user_data, const char *title);
    void (*resize_window)(void *user_data, int cols, int lines);
    void (*ring_bell)(void *user_data, bool visual);

    // I/O: core asks host to send bytes to the remote peer
    int (*send_data)(void *user_data, const uint8_t *data, size_t len);

    // OS integration
    void (*set_clipboard)(void *user_data, const char *text);
    void (*set_ime_status)(void *user_data, bool enable);
} ttcore_callbacks_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create a terminal engine instance.
// cols/lines: initial terminal dimensions (minimum 1×1).
// cb: UI callbacks (copied internally; may be NULL for headless use).
// Returns NULL on allocation failure.
ttcore_t *ttcore_create(int cols, int lines, ttcore_callbacks_t *cb);

// Like ttcore_create but with an explicit scrollback buffer.
// scrollback_lines: extra lines above the visible screen stored in the ring
//   buffer (0 = no scrollback, same as ttcore_create).
// Enables ttcore_scroll_to() / ttcore_get_scroll_max().
ttcore_t *ttcore_create_ex(int cols, int lines, int scrollback_lines,
                            ttcore_callbacks_t *cb);

// Destroy an instance.  NULL-safe.
void ttcore_destroy(ttcore_t *tc);

// ---------------------------------------------------------------------------
// Data flow
// ---------------------------------------------------------------------------

// Feed raw bytes received from the remote host (PTY / network).
void ttcore_parse_data(ttcore_t *tc, const uint8_t *data, size_t len);

// Notify the engine that the terminal window was resized.
// Reallocates the buffer (content-copy), resets margins and tab stops.
// Scrollback capacity (set via ttcore_create_ex) is preserved.
void ttcore_resize(ttcore_t *tc, int cols, int lines);

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

// Current cursor position (0-based column / row).
void ttcore_get_cursor_pos(const ttcore_t *tc, int *x, int *y);

// Terminal dimensions.
int ttcore_get_cols(const ttcore_t *tc);
int ttcore_get_lines(const ttcore_t *tc);

// Cell access — returns a pointer into the internal buffer.
// Cast to (const buff_char_t *) after including ttcore_buffer.h.
// Returns NULL if (x,y) is out of range.
const void *ttcore_get_cell(const ttcore_t *tc, int x, int y);

// ---------------------------------------------------------------------------
// Scrollback navigation
// ---------------------------------------------------------------------------

// Scroll the viewport relative to the live (bottom) position.
// offset = 0  → live view.
// offset = N  → N lines above live; clamped to [0, ttcore_get_scroll_max()].
// Takes effect immediately: the next ttcore_get_cell() calls reflect the
// new view.  When new data causes a BuffScroll, the viewport is automatically
// compensated (TeraTerm WinOrgY model) so the visible content stays stable.
void ttcore_scroll_to(ttcore_t *tc, int offset);

// Returns the number of scrollback lines currently available above the live
// screen.  This is the maximum valid offset for ttcore_scroll_to().
// Returns 0 if no scrollback was configured (ttcore_create) or the buffer
// has not yet accumulated any history.
int ttcore_get_scroll_max(const ttcore_t *tc);

// Returns the current viewport offset: 0 = live view, N = N lines above live.
// Stays valid across BuffScroll events — the compensation in BuffScroll keeps
// win_org_y (and therefore get_scroll_pos) correct without any Python-side
// bookkeeping.  Use this after ttcore_parse_data() to sync the UI scroll state.
int ttcore_get_scroll_pos(const ttcore_t *tc);

// ---------------------------------------------------------------------------
// ctypes integration helper
// ---------------------------------------------------------------------------

// Returns sizeof(buff_char_t) at C runtime.
// Python host: assert ctypes.sizeof(BuffChar) == ttcore_sizeof_cell()
// to validate that the ctypes Structure layout matches the C struct.
size_t ttcore_sizeof_cell(void);

// ---------------------------------------------------------------------------
// Charset
// ---------------------------------------------------------------------------

// Change the active input encoding at runtime.
// encoding is a VtCharsetEncoding value from vtcharset.h.
void ttcore_set_encoding(ttcore_t *tc, int encoding);

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

// Forward a host mouse event to the engine.
// event/button/modifiers: VtMouseEv_*, VTMOUSE_BTN_*, VTMOUSE_MOD_* from vtmouse.h.
// x, y: 1-based screen column/row.
// Returns true if a report was generated and sent to the remote host.
bool ttcore_mouse_event(ttcore_t *tc, int event, int button,
                        int x, int y, uint8_t modifiers);

#ifdef __cplusplus
}
#endif

#endif  // LIBTTCORE_H_
