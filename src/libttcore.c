// 2026-03-17 00:00 v0.2.3
// libttcore.c — Public API implementation: ttcore_buffer + vtparse + vtcharset
//               + vtmouse + vtcolor behind a single opaque handle.
// Win32-free, C99, zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "libttcore.h"
#include "ttcore_buffer.h"
#include "vtparse.h"
#include "vtcharset.h"
#include "vtmouse.h"
#include "vtcolor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Private implementation struct — never exposed in the header
// ---------------------------------------------------------------------------

struct ttcore_t {
    ttcore_buffer_t   *buf;       // Screen buffer + current cursor state
    VtParser          *parser;    // VT100/VT220/xterm state machine
    VtCharsetCtx      *charset;   // Charset decoder (UTF-8/SJIS/EUC/…)
    VtMouseCtx        *mouse;     // Mouse report encoder

    int                cols;
    int                lines;

    ttcore_callbacks_t cb;        // User-supplied UI callbacks (copied)
};

// ---------------------------------------------------------------------------
// Buffer addressing helper — mirrors the static GetLinePtr in ttcore_buffer.c
// ---------------------------------------------------------------------------

static int32_t buf_line_idx(const ttcore_buffer_t *buf, int screen_y) {
    int32_t ptr = (int32_t)(buf->BuffStartAbs + buf->PageStart + screen_y)
                  * buf->NumOfColumns;
    while (ptr >= buf->BufferSize) ptr -= buf->BufferSize;
    while (ptr < 0)                ptr += buf->BufferSize;
    return ptr;
}

// Viewport-adjusted line index for rendering.  Includes win_org_y so
// that scrolled-back views read from the correct history lines.
// MUST NOT be used for write operations (buf_write_char, NewLine, etc.).
static int32_t buf_view_line_idx(const ttcore_buffer_t *buf, int screen_y) {
    int32_t ptr = (int32_t)(buf->BuffStartAbs + buf->PageStart
                            + buf->win_org_y + screen_y)
                  * buf->NumOfColumns;
    while (ptr >= buf->BufferSize) ptr -= buf->BufferSize;
    while (ptr < 0)                ptr += buf->BufferSize;
    return ptr;
}

// ---------------------------------------------------------------------------
// Write one Unicode codepoint to the buffer at the current cursor position.
// Does NOT advance the cursor (handled by charset_put_char / on_put_char).
// ---------------------------------------------------------------------------

static void buf_write_char(ttcore_t *tc, uint32_t u32) {
    ttcore_buffer_t *b = tc->buf;
    if (b->CursorX < 0 || b->CursorX >= b->NumOfColumns) return;
    if (b->CursorY < 0 || b->CursorY >= b->NumOfLines)   return;

    int32_t ptr = buf_line_idx(b, b->CursorY);
    b->LinePtr   = ptr;   // keep LinePtr in sync (used by buffer ops)

    buff_char_t *cell = &b->CodeBuffW[ptr + b->CursorX];
    cell->u32       = u32;
    cell->ansi_char = (u32 < 0x80u) ? (uint16_t)u32 : 0u;
    cell->attr      = b->CurCharAttr.Attr;
    cell->attr2     = b->CurCharAttr.Attr2;
    cell->fg        = b->CurCharAttr.Fore;
    cell->bg        = b->CurCharAttr.Back;
}

// ---------------------------------------------------------------------------
// Internal line-feed (no vtparse involvement) — used by cursor wrap and LF CB
// ---------------------------------------------------------------------------

static void do_line_feed(ttcore_t *tc) {
    ttcore_buffer_t *b = tc->buf;
    if (b->CursorY >= b->CursorBottom) {
        ttcore_buffer_scroll_n_lines(b, 1, b->CursorBottom);
    } else {
        b->CursorY++;
    }
}

// ===========================================================================
// vtcharset callbacks
// ===========================================================================

// Decoded Unicode codepoint from vtcharset → write to buffer, advance cursor.
static void charset_put_char(uint32_t u32, void *cd) {
    ttcore_t *tc = (ttcore_t *)cd;
    buf_write_char(tc, u32);

    int new_x = tc->buf->CursorX + 1;
    if (new_x >= tc->buf->NumOfColumns) {
        if (tc->buf->Wrap) {
            tc->buf->CursorX = tc->buf->CursorLeftM;
            do_line_feed(tc);
        } else {
            tc->buf->CursorX = tc->buf->NumOfColumns - 1;
        }
    } else {
        tc->buf->CursorX = new_x;
    }
}

// C0/C1 control bytes from vtcharset — vtparse owns control handling, no-op.
static void charset_put_ctrl(uint8_t b, void *cd) {
    (void)b; (void)cd;
}

// ===========================================================================
// vtmouse callback
// ===========================================================================

static void mouse_write_report(const char *buf, int len, void *cd) {
    ttcore_t *tc = (ttcore_t *)cd;
    if (tc->cb.send_data)
        tc->cb.send_data(tc->cb.user_data, (const uint8_t *)buf, (size_t)len);
}

// ===========================================================================
// vtparse callbacks — one static function per event
// All retrieve ttcore_t* via VtParserGetUserData(p).
// ===========================================================================

// --- I/O ---

static void on_write_output(VtParser *p, const uint8_t *data, size_t len) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->cb.send_data)
        tc->cb.send_data(tc->cb.user_data, data, len);
}

// --- Character output (raw byte from vtparse → vtcharset → buffer) ---

static void on_put_char(VtParser *p, uint32_t raw_byte,
                        const VtCharAttr *attr) {
    ttcore_t *tc = VtParserGetUserData(p);
    (void)attr;   // char attr already applied via on_set_char_attr
    VtCharsetFeedByte(tc->charset, (uint8_t)(raw_byte & 0xFFu));
}

// --- Cursor: position query (CRITICAL — used by SaveCursorToSlot / DSR) ---

static void on_get_cursor_pos(VtParser *p, int *x, int *y) {
    ttcore_t *tc = VtParserGetUserData(p);
    *x = tc->buf->CursorX;
    *y = tc->buf->CursorY;
}

static void on_get_screen_size(VtParser *p, int *cols, int *rows) {
    ttcore_t *tc = VtParserGetUserData(p);
    *cols = tc->cols;
    *rows = tc->lines;
}

// --- Cursor: movement ---

static void on_cursor_move(VtParser *p, int x, int y) {
    ttcore_t *tc = VtParserGetUserData(p);
    int max_x = tc->buf->NumOfColumns - 1;
    int max_y = tc->buf->NumOfLines - tc->buf->StatusLine - 1;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    tc->buf->CursorX = x;
    tc->buf->CursorY = y;
    if (tc->cb.set_cursor_pos)
        tc->cb.set_cursor_pos(tc->cb.user_data, x, y);
}

static void on_cursor_up(VtParser *p, int n, bool affect_margin) {
    ttcore_t *tc = VtParserGetUserData(p);
    int top = affect_margin ? tc->buf->CursorTop : 0;
    int y = tc->buf->CursorY - n;
    tc->buf->CursorY = (y < top) ? top : y;
}

static void on_cursor_down(VtParser *p, int n, bool affect_margin) {
    ttcore_t *tc = VtParserGetUserData(p);
    int bot = affect_margin ? tc->buf->CursorBottom
                            : tc->buf->NumOfLines - tc->buf->StatusLine - 1;
    int y = tc->buf->CursorY + n;
    tc->buf->CursorY = (y > bot) ? bot : y;
}

static void on_cursor_right(VtParser *p, int n, bool affect_margin) {
    ttcore_t *tc = VtParserGetUserData(p);
    int right = affect_margin ? tc->buf->CursorRightM
                              : tc->buf->NumOfColumns - 1;
    int x = tc->buf->CursorX + n;
    tc->buf->CursorX = (x > right) ? right : x;
}

static void on_cursor_left(VtParser *p, int n, bool affect_margin) {
    ttcore_t *tc = VtParserGetUserData(p);
    int left = affect_margin ? tc->buf->CursorLeftM : 0;
    int x = tc->buf->CursorX - n;
    tc->buf->CursorX = (x < left) ? left : x;
}

static void on_cursor_set_style(VtParser *p, int style, bool blink) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->cb.set_cursor_style)
        tc->cb.set_cursor_style(tc->cb.user_data, style, blink);
}

// --- Cursor: save/restore (notifications only — vtparse owns the state) ---
// vtparse fires cursor_move + set_char_attr after restore: no extra work here.
// These are never actually called by vtparse (confirmed in source), but wired
// defensively in case future vtparse versions use them.
static void on_cursor_save(VtParser *p)    { (void)p; }
static void on_cursor_restore(VtParser *p) { (void)p; }

// --- Screen: scroll primitives ---

static void on_line_feed(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    do_line_feed(tc);
}

static void on_reverse_index(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->buf->CursorY <= tc->buf->CursorTop) {
        // Insert blank line at top of scroll region (scroll content down)
        ttcore_buffer_insert_lines(tc->buf, 1,
                                   tc->buf->CursorBottom,
                                   tc->buf->CursorTop);
    } else {
        tc->buf->CursorY--;
    }
}

static void on_carriage_return(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    tc->buf->CursorX = tc->buf->CursorLeftM;
}

static void on_back_space(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->buf->CursorX > tc->buf->CursorLeftM) tc->buf->CursorX--;
}

// --- Screen: erase ---

static void on_erase_line(VtParser *p, int mode) {
    ttcore_t *tc = VtParserGetUserData(p);
    int x = tc->buf->CursorX, y = tc->buf->CursorY;
    switch (mode) {
    case VTPARSER_ERASE_TO_END:
        ttcore_buffer_erase_cur_to_end(tc->buf, x, y);
        break;
    case VTPARSER_ERASE_TO_BEGIN:
        ttcore_buffer_erase_home_to_cur(tc->buf, x, y, false);
        break;
    case VTPARSER_ERASE_ALL:
        ttcore_buffer_erase_chars_in_line(tc->buf, 0, tc->buf->NumOfColumns,
                                          x, y, false);
        break;
    }
}

static void on_erase_display(VtParser *p, int mode) {
    ttcore_t *tc = VtParserGetUserData(p);
    int x = tc->buf->CursorX, y = tc->buf->CursorY;
    int last_y = tc->buf->NumOfLines - tc->buf->StatusLine - 1;
    switch (mode) {
    case VTPARSER_ERASE_TO_END:
        ttcore_buffer_erase_cur_to_end(tc->buf, x, y);
        for (int i = y + 1; i <= last_y; i++)
            ttcore_buffer_erase_chars_in_line(tc->buf, 0,
                                              tc->buf->NumOfColumns, 0, i, false);
        break;
    case VTPARSER_ERASE_TO_BEGIN:
        ttcore_buffer_erase_home_to_cur(tc->buf, x, y, false);
        for (int i = 0; i < y; i++)
            ttcore_buffer_erase_chars_in_line(tc->buf, 0,
                                              tc->buf->NumOfColumns, 0, i, false);
        break;
    case VTPARSER_ERASE_ALL:
        for (int i = 0; i <= last_y; i++)
            ttcore_buffer_erase_chars_in_line(tc->buf, 0,
                                              tc->buf->NumOfColumns, 0, i, false);
        if (tc->cb.clear_screen)
            tc->cb.clear_screen(tc->cb.user_data);
        break;
    case VTPARSER_ERASE_SAVED:
        break;   // no scrollback management in Phase 4
    }
}

static void on_erase_chars(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_erase_chars(tc->buf, n,
                               tc->buf->CursorX, tc->buf->CursorY, false);
}

static void on_fill_with_e(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_fill_with_e(tc->buf);
}

// --- Screen: insert / delete ---

static void on_insert_lines(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_insert_lines(tc->buf, n,
                               tc->buf->CursorBottom, tc->buf->CursorY);
}

static void on_delete_lines(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_delete_lines(tc->buf, n,
                               tc->buf->CursorBottom, tc->buf->CursorY);
}

static void on_insert_chars(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_insert_space(tc->buf, n,
                               tc->buf->CursorX, tc->buf->CursorY);
}

static void on_delete_chars(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_delete_chars(tc->buf, n,
                               tc->buf->CursorX, tc->buf->CursorY);
}

static void on_repeat_char(VtParser *p, int n) {
    // REP: repeat last printed character n times via vtcharset
    ttcore_t *tc = VtParserGetUserData(p);
    // last_put_character is private in vtparse — approximate via u32 in cell
    int x = tc->buf->CursorX > 0 ? tc->buf->CursorX - 1 : 0;
    int32_t idx = buf_line_idx(tc->buf, tc->buf->CursorY) + x;
    uint32_t last = tc->buf->CodeBuffW[idx].u32;
    for (int i = 0; i < n; i++) charset_put_char(last, tc);
}

// --- Screen: scroll ---

static void on_scroll_up(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_scroll_n_lines(tc->buf, n, tc->buf->CursorBottom);
}

static void on_scroll_down(VtParser *p, int n) {
    // SD: insert n blank lines at CursorTop, shifting region downward
    ttcore_t *tc = VtParserGetUserData(p);
    ttcore_buffer_insert_lines(tc->buf, n,
                               tc->buf->CursorBottom, tc->buf->CursorTop);
}

// --- Screen: scroll region ---

static void on_set_scroll_region(VtParser *p, int top, int bot) {
    ttcore_t *tc = VtParserGetUserData(p);
    tc->buf->CursorTop    = top;
    tc->buf->CursorBottom = bot;
}

static void on_set_lr_scroll_region(VtParser *p, int left, int right) {
    ttcore_t *tc = VtParserGetUserData(p);
    tc->buf->CursorLeftM  = left;
    tc->buf->CursorRightM = right;
}

// --- Screen: tab stops (simplified: default 8-column grid) ---

static void on_forward_tab(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    int x = tc->buf->CursorX;
    for (int i = 0; i < n; i++) {
        x = (x / 8 + 1) * 8;
        if (x >= tc->buf->NumOfColumns) { x = tc->buf->NumOfColumns - 1; break; }
    }
    tc->buf->CursorX = x;
}

static void on_backward_tab(VtParser *p, int n) {
    ttcore_t *tc = VtParserGetUserData(p);
    int x = tc->buf->CursorX;
    for (int i = 0; i < n; i++) {
        x = ((x - 1) / 8) * 8;
        if (x < 0) { x = 0; break; }
    }
    tc->buf->CursorX = x;
}

static void on_set_tab_stop(VtParser *p)        { (void)p; }
static void on_clear_tab_stop(VtParser *p, int m) { (void)p; (void)m; }

// --- Screen: attributes ---

static void on_set_char_attr(VtParser *p, const VtCharAttr *va) {
    ttcore_t *tc = VtParserGetUserData(p);
    uint8_t attr = ATTR_DEFAULT;
    if (va->bold)      attr |= ATTR_BOLD;
    if (va->underline) attr |= ATTR_UNDER;
    if (va->blink)     attr |= ATTR_BLINK;
    if (va->reverse)   attr |= ATTR_REVERSE;
    tc->buf->CurCharAttr.Attr = attr;

    /* Decode VtCharAttr colour encoding (set by vtparse.c):
     *   0x01000000          VTPARSER_COLOR_DEFAULT  — use terminal default
     *   0x03000000 | idx    ANSI 16-colour index    — SGR 30-37 / 90-97
     *   0x02000000 | idx    256-colour index        — SGR 38;5;idx / 48;5;idx
     *   0x00rrggbb          RGB24                   — SGR 38;2;r;g;b
     * The original code cast every value to VtRgb24 and called
     * VtColorFindClosest(), which turned 0x03000002 (green, idx=2) into
     * rgb(3,0,2) → nearest palette entry = 0 (black).  Fixed below.
     */
    uint8_t attr2 = 0;
    if (va->fg != VTPARSER_COLOR_DEFAULT) {
        uint32_t fg  = va->fg;
        uint32_t tag = fg & 0xFF000000u;
        if (tag == 0x03000000u || tag == 0x02000000u) {
            /* palette index is in the low byte */
            tc->buf->CurCharAttr.Fore = (uint8_t)(fg & 0xFFu);
        } else {
            /* RGB24 — find nearest entry in 256-colour palette */
            tc->buf->CurCharAttr.Fore =
                (uint8_t)VtColorFindClosest((VtRgb24)fg, NULL);
        }
        attr2 |= ATTR2_FORE;
    } else {
        tc->buf->CurCharAttr.Fore = ATTR_DEFAULT_FG;
    }
    if (va->bg != VTPARSER_COLOR_DEFAULT) {
        uint32_t bg  = va->bg;
        uint32_t tag = bg & 0xFF000000u;
        if (tag == 0x03000000u || tag == 0x02000000u) {
            tc->buf->CurCharAttr.Back = (uint8_t)(bg & 0xFFu);
        } else {
            tc->buf->CurCharAttr.Back =
                (uint8_t)VtColorFindClosest((VtRgb24)bg, NULL);
        }
        attr2 |= ATTR2_BACK;
    } else {
        tc->buf->CurCharAttr.Back = ATTR_DEFAULT_BG;
    }
    tc->buf->CurCharAttr.Attr2 = attr2;
}

// --- Screen: misc ---

static void on_update_screen(VtParser *p) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->cb.update_rect)
        tc->cb.update_rect(tc->cb.user_data, 0, 0, tc->cols, tc->lines);
}

static void on_switch_screen(VtParser *p, bool alt) {
    // Phase 4: simplified — reset cursor; full alt-screen buffer is Phase 5
    ttcore_t *tc = VtParserGetUserData(p);
    tc->buf->CursorX = 0;
    tc->buf->CursorY = 0;
    (void)alt;
}

// --- Terminal state ---

static void on_bell(VtParser *p, int type) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->cb.ring_bell)
        tc->cb.ring_bell(tc->cb.user_data, type == VTPARSER_BEEP_VISUAL);
}

static void on_title_change(VtParser *p, const char *title) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (tc->cb.set_window_title)
        tc->cb.set_window_title(tc->cb.user_data, title);
}

static void on_icon_change(VtParser *p, const char *name) {
    (void)p; (void)name;   // no icon callback in ttcore_callbacks_t
}

static void on_resize(VtParser *p, int cols, int rows) {
    ttcore_t *tc = VtParserGetUserData(p);
    tc->cols  = cols;
    tc->lines = rows;
    if (tc->cb.resize_window)
        tc->cb.resize_window(tc->cb.user_data, cols, rows);
}

static void on_terminal_id_change(VtParser *p, int new_id) {
    (void)p; (void)new_id;
}

// --- Mouse ---

static void on_set_mouse_mode(VtParser *p, int mode, int ext_mode) {
    ttcore_t *tc = VtParserGetUserData(p);
    VtMouseSetMode(tc->mouse, (VtMouseMode)mode, (VtMouseExtMode)ext_mode);
}

// --- Status line ---

static void on_hide_status_line(VtParser *p)           { (void)p; }
static void on_show_status_line(VtParser *p, int type) { (void)p; (void)type; }

// --- TEK ---

static void on_enter_tek_mode(VtParser *p) { (void)p; }

// --- Color ---

static void on_set_color(VtParser *p, int slot, uint32_t rgb) {
    (void)p; (void)slot; (void)rgb;   // color palette: Phase 5
}

static void on_reset_color(VtParser *p, int slot) {
    (void)p; (void)slot;
}

static void on_query_color(VtParser *p, int slot) {
    // Respond with current palette entry via write_output
    ttcore_t *tc = VtParserGetUserData(p);
    const VtRgb24 *pal = VtColorDefaultPalette();
    if (slot < 0 || slot > 255) return;
    VtRgb24 rgb = pal[slot];
    char resp[64];
    int n = snprintf(resp, sizeof(resp),
                     "\033]4;%d;rgb:%04x/%04x/%04x\x07",
                     slot,
                     (unsigned)VT_R(rgb) * 0x101u,
                     (unsigned)VT_G(rgb) * 0x101u,
                     (unsigned)VT_B(rgb) * 0x101u);
    if (n > 0 && tc->cb.send_data)
        tc->cb.send_data(tc->cb.user_data, (const uint8_t *)resp, (size_t)n);
}

// --- Window operations ---

static void on_window_op(VtParser *p, int op, int p1, int p2) {
    ttcore_t *tc = VtParserGetUserData(p);
    if (op == 8 && tc->cb.resize_window)   // op 8 = resize in chars
        tc->cb.resize_window(tc->cb.user_data, p2, p1);
}

// --- User-defined keys ---

static void on_define_user_key(VtParser *p, int id,
                                const uint8_t *str, int len) {
    (void)p; (void)id; (void)str; (void)len;
}

// --- Logging ---

static void on_log_byte(VtParser *p, uint8_t b)   { (void)p; (void)b; }
static void on_log_char32(VtParser *p, uint32_t c) { (void)p; (void)c; }

// --- Insert byte (push-back into input) ---

static void on_insert_byte(VtParser *p, uint8_t b) { (void)p; (void)b; }

// ===========================================================================
// Callback table builder — fills every slot; NULL means fire-and-forget
// ===========================================================================

static VtParserCallbacks build_callbacks(void) {
    VtParserCallbacks cb;
    memset(&cb, 0, sizeof(cb));

    cb.write_output         = on_write_output;
    cb.insert_byte          = on_insert_byte;
    cb.put_char             = on_put_char;

    cb.cursor_move          = on_cursor_move;
    cb.cursor_up            = on_cursor_up;
    cb.cursor_down          = on_cursor_down;
    cb.cursor_right         = on_cursor_right;
    cb.cursor_left          = on_cursor_left;
    cb.cursor_save          = on_cursor_save;
    cb.cursor_restore       = on_cursor_restore;
    cb.cursor_set_style     = on_cursor_set_style;

    cb.erase_line           = on_erase_line;
    cb.erase_display        = on_erase_display;
    cb.erase_chars          = on_erase_chars;
    cb.fill_with_e          = on_fill_with_e;

    cb.insert_lines         = on_insert_lines;
    cb.delete_lines         = on_delete_lines;
    cb.insert_chars         = on_insert_chars;
    cb.delete_chars         = on_delete_chars;
    cb.repeat_char          = on_repeat_char;

    cb.scroll_up            = on_scroll_up;
    cb.scroll_down          = on_scroll_down;
    cb.set_scroll_region    = on_set_scroll_region;
    cb.set_lr_scroll_region = on_set_lr_scroll_region;

    cb.set_tab_stop         = on_set_tab_stop;
    cb.clear_tab_stop       = on_clear_tab_stop;
    cb.forward_tab          = on_forward_tab;
    cb.backward_tab         = on_backward_tab;

    cb.set_char_attr        = on_set_char_attr;

    cb.reverse_index        = on_reverse_index;
    cb.line_feed            = on_line_feed;
    cb.carriage_return      = on_carriage_return;
    cb.back_space           = on_back_space;
    cb.update_screen        = on_update_screen;

    cb.set_color            = on_set_color;
    cb.reset_color          = on_reset_color;
    cb.query_color          = on_query_color;

    cb.on_bell              = on_bell;
    cb.on_title_change      = on_title_change;
    cb.on_icon_change       = on_icon_change;
    cb.on_resize            = on_resize;
    cb.on_terminal_id_change = on_terminal_id_change;

    cb.switch_screen        = on_switch_screen;
    cb.set_appli_keypad     = NULL;   // key translation: Phase 5
    cb.set_appli_cursor     = NULL;
    cb.define_user_key      = on_define_user_key;

    cb.set_mouse_mode       = on_set_mouse_mode;

    cb.hide_status_line     = on_hide_status_line;
    cb.show_status_line     = on_show_status_line;

    cb.enter_tek_mode       = on_enter_tek_mode;

    cb.get_cursor_pos       = on_get_cursor_pos;
    cb.get_screen_size      = on_get_screen_size;

    cb.log_byte             = on_log_byte;
    cb.log_char32           = on_log_char32;

    cb.window_op            = on_window_op;

    return cb;
}

// ===========================================================================
// Public API
// ===========================================================================

ttcore_t *ttcore_create_ex(int cols, int lines, int scrollback_lines,
                            ttcore_callbacks_t *cb) {
    if (cols < 1) cols = 80;
    if (lines < 1) lines = 24;
    if (scrollback_lines < 0) scrollback_lines = 0;

    ttcore_t *tc = (ttcore_t *)calloc(1, sizeof(ttcore_t));
    if (!tc) return NULL;

    tc->cols  = cols;
    tc->lines = lines;
    if (cb) tc->cb = *cb;

    // 1. Screen buffer (with optional scrollback)
    tc->buf = ttcore_buffer_create(cols, lines, scrollback_lines, &tc->cb);
    if (!tc->buf) goto fail;
    tc->buf->CursorBottom = lines - 1;
    tc->buf->CursorRightM = cols  - 1;

    // 2. Charset decoder — default: UTF-8
    static const VtCharsetOps charset_ops = { charset_put_char, charset_put_ctrl };
    VtCharsetConfig charset_cfg;
    memset(&charset_cfg, 0, sizeof(charset_cfg));
    charset_cfg.encoding = VtCharsetEnc_UTF8;
    tc->charset = VtCharsetInit(&charset_ops, tc, &charset_cfg);
    if (!tc->charset) goto fail;

    // 3. Mouse encoder
    static const VtMouseOps mouse_ops = { mouse_write_report };
    tc->mouse = VtMouseInit(&mouse_ops, tc);
    if (!tc->mouse) goto fail;

    // 4. VT parser
    VtParserConfig pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.terminal_id         = VTPARSER_ID_VT220;
    /* In UTF-8 mode the byte range 0x80-0x9F carries continuation bytes, so
     * 8-bit C1 controls must be disabled (C1 arrives as 7-bit ESC sequences).
     * For non-UTF-8 encodings (SJIS, EUC, ISO 8859, …) C1 can be accepted. */
    pcfg.accept_8bit_ctrl    = (charset_cfg.encoding != VtCharsetEnc_UTF8);
    pcfg.osc_buffer_max      = VTPARSER_OSC_BUF_MAX;
    pcfg.beep_mode           = VTPARSER_BEEP_ON;
    pcfg.accept_title_change = true;

    VtParserCallbacks pcbs = build_callbacks();
    tc->parser = VtParserCreate(&pcfg, &pcbs, tc);
    if (!tc->parser) goto fail;

    return tc;

fail:
    ttcore_destroy(tc);
    return NULL;
}

ttcore_t *ttcore_create(int cols, int lines, ttcore_callbacks_t *cb) {
    return ttcore_create_ex(cols, lines, 0, cb);
}

void ttcore_destroy(ttcore_t *tc) {
    if (!tc) return;
    VtParserDestroy(tc->parser);
    VtMouseFinish(tc->mouse);
    VtCharsetFinish(tc->charset);
    ttcore_buffer_destroy(tc->buf);
    free(tc);
}

void ttcore_parse_data(ttcore_t *tc, const uint8_t *data, size_t len) {
    if (!tc || !data || len == 0) return;
    VtParserInput(tc->parser, data, len);
}

void ttcore_resize(ttcore_t *tc, int cols, int lines) {
    if (!tc) return;
    if (cols < 1) cols = 1;
    if (lines < 1) lines = 1;

    /* Save absolute cursor position before resize.  CursorY is relative to
     * PageStart; ChangeBuffer() inside ttcore_buffer_init() recomputes
     * PageStart = BuffEnd - NumOfLines, so CursorY must be adjusted. */
    int abs_cursor_y = tc->buf->PageStart + tc->buf->CursorY;

    tc->cols  = cols;
    tc->lines = lines;
    ttcore_buffer_init(tc->buf, cols, lines);   /* content-copy; preserves ScrollBack */
    ttcore_buffer_reset(tc->buf);               /* re-establishes margins, tab stops  */

    /* Restore cursor position relative to new PageStart. */
    int new_cy = abs_cursor_y - tc->buf->PageStart;
    if (new_cy < 0) new_cy = 0;
    if (new_cy >= lines) new_cy = lines - 1;
    tc->buf->CursorY = new_cy;

    /* Clamp CursorX to new column count. */
    if (tc->buf->CursorX >= cols) tc->buf->CursorX = cols - 1;
}

void ttcore_get_cursor_pos(const ttcore_t *tc, int *x, int *y) {
    if (!tc) { if (x) *x = 0; if (y) *y = 0; return; }
    if (x) *x = tc->buf->CursorX;
    if (y) *y = tc->buf->CursorY;
}

int ttcore_get_cols(const ttcore_t *tc) {
    return tc ? tc->cols : 0;
}

int ttcore_get_lines(const ttcore_t *tc) {
    return tc ? tc->lines : 0;
}

const void *ttcore_get_cell(const ttcore_t *tc, int x, int y) {
    if (!tc) return NULL;
    if (x < 0 || y < 0) return NULL;
    if (x >= tc->buf->NumOfColumns) return NULL;
    if (y >= tc->buf->NumOfLines - tc->buf->StatusLine) return NULL;
    int32_t idx = buf_view_line_idx(tc->buf, y) + x;
    return &tc->buf->CodeBuffW[idx];
}

void ttcore_set_encoding(ttcore_t *tc, int encoding) {
    if (!tc) return;
    VtCharsetConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.encoding = (VtCharsetEncoding)encoding;
    VtCharsetSetConfig(tc->charset, &cfg);
}

bool ttcore_mouse_event(ttcore_t *tc, int event, int button,
                        int x, int y, uint8_t modifiers) {
    if (!tc) return false;
    return VtMouseReport(tc->mouse, (VtMouseEvent)event, button,
                         x, y, modifiers);
}

void ttcore_scroll_to(ttcore_t *tc, int offset) {
    if (!tc) return;
    ttcore_buffer_scroll_to(tc->buf, offset);
}

int ttcore_get_scroll_max(const ttcore_t *tc) {
    if (!tc) return 0;
    return ttcore_buffer_get_scroll_max(tc->buf);
}

int ttcore_get_scroll_pos(const ttcore_t *tc) {
    if (!tc) return 0;
    return ttcore_buffer_get_scroll_pos(tc->buf);
}

size_t ttcore_sizeof_cell(void) {
    return sizeof(buff_char_t);
}
