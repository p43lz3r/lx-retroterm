// 2026-03-22 v0.6.1
// terminal_view.c — GtkDrawingArea with libttcore rendering via Cairo+Pango.
// Clear Screen (ESC[2J + ESC[H via ttcore_parse_data).
// Phase G10b: mouse selection + Copy to clipboard (Ctrl+Shift+C, auto-copy).
// Phase G5: cursor styles (block/underline, blink, DECSCUSR), focus tracking.
// Fix: compute font metrics on first configure-event so terminal size matches
//      widget from startup; reset g_scroll_offset on resize (stale win_org_y).
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "terminal_view.h"
#include "view_settings.h"
#include "status_bar.h"
#include "serial_conn.h"

#include "libttcore.h"
#include "ttcore_buffer.h"
#include "vtparse.h"
#include "vtcolor.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static ttcore_t *g_tc = NULL;
static GtkWidget *g_drawing_area = NULL;
static PangoFontDescription *g_font_desc = NULL;
static int g_cell_width = 0;
static int g_cell_height = 0;
static int g_font_ascent = 0;
static int g_cur_cols = 80;
static int g_cur_lines = 24;
static bool g_resize_suppressed = false;
static int g_pending_cols = 0;
static int g_pending_lines = 0;
static bool g_has_pending_resize = false;
static guint g_resize_timer_id = 0;
static int g_deferred_cols = 0;
static int g_deferred_lines = 0;
static int g_scroll_offset = 0;
static GtkWidget *g_scrollbar = NULL;
static GtkAdjustment *g_scroll_adj = NULL;
static gulong g_adj_changed_id = 0;
static bool g_adj_updating = false;

// Backspace key behavior: false=BS(0x08), true=DEL(0x7F)
static bool g_backspace_del = false;

// Cursor style state (G5)
static int g_cursor_shape = VTPARSER_CURSOR_BLOCK;
static bool g_cursor_blink = false;
static bool g_cursor_visible = true;
static guint g_blink_timer_id = 0;
static bool g_has_focus = true;

// Selection state (G10b)
static bool g_selecting = false;      // mouse drag in progress
static bool g_has_selection = false;   // selection exists (highlight + copy)
static int g_sel_start_col = 0;
static int g_sel_start_row = 0;
static int g_sel_end_col = 0;
static int g_sel_end_row = 0;

// ---------------------------------------------------------------------------
// Cursor blink timer (G5)
// ---------------------------------------------------------------------------

static gboolean on_blink_tick(gpointer user_data) {
    (void)user_data;
    g_cursor_visible = !g_cursor_visible;
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    return G_SOURCE_CONTINUE;
}

static void update_blink_timer(void) {
    if (g_blink_timer_id) {
        g_source_remove(g_blink_timer_id);
        g_blink_timer_id = 0;
    }
    if (g_cursor_blink && g_has_focus) {
        g_cursor_visible = true;
        g_blink_timer_id = g_timeout_add(530, on_blink_tick, NULL);
    } else {
        g_cursor_visible = true;
    }
}

// ---------------------------------------------------------------------------
// Cursor shape helpers (G5)
// ---------------------------------------------------------------------------

// Returns true if the shape is an underline (or bar mapped to underline).
static bool cursor_is_underline(int shape) {
    return shape == VTPARSER_CURSOR_UNDERLINE_BLINK
        || shape == VTPARSER_CURSOR_UNDERLINE_STEADY
        || shape == VTPARSER_CURSOR_BAR_BLINK
        || shape == VTPARSER_CURSOR_BAR_STEADY;
}

// ---------------------------------------------------------------------------
// DECSCUSR callback — called by libttcore when remote sends cursor style
// ---------------------------------------------------------------------------

static void on_cursor_style_cb(void *user_data, int shape, bool blink) {
    (void)user_data;
    // shape == -1 means "keep current shape" (ATT610 blink toggle)
    if (shape >= 0) g_cursor_shape = shape;
    g_cursor_blink = blink;
    update_blink_timer();
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
}

// ---------------------------------------------------------------------------
// Focus event handlers (G5)
// ---------------------------------------------------------------------------

static gboolean on_focus_in(GtkWidget *widget, GdkEventFocus *event,
                             gpointer user_data) {
    (void)widget;
    (void)event;
    (void)user_data;
    g_has_focus = true;
    update_blink_timer();
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    return FALSE;
}

static gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *event,
                              gpointer user_data) {
    (void)widget;
    (void)event;
    (void)user_data;
    g_has_focus = false;
    update_blink_timer();
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    return FALSE;
}

// ---------------------------------------------------------------------------
// Palette lookup: cell index -> GdkRGBA
// ---------------------------------------------------------------------------

static void palette_to_rgba(uint8_t idx, GdkRGBA *out) {
    const VtRgb24 *pal = VtColorDefaultPalette();
    VtRgb24 rgb = pal[idx];
    out->red   = VT_R(rgb) / 255.0;
    out->green = VT_G(rgb) / 255.0;
    out->blue  = VT_B(rgb) / 255.0;
    out->alpha = 1.0;
}

// ---------------------------------------------------------------------------
// Selection helpers (G10b)
// ---------------------------------------------------------------------------

// Normalize selection so (r1,c1) <= (r2,c2) in reading order.
static void sel_normalize(int *r1, int *c1, int *r2, int *c2) {
    int sr = g_sel_start_row, sc = g_sel_start_col;
    int er = g_sel_end_row, ec = g_sel_end_col;
    if (sr > er || (sr == er && sc > ec)) {
        *r1 = er; *c1 = ec;
        *r2 = sr; *c2 = sc;
    } else {
        *r1 = sr; *c1 = sc;
        *r2 = er; *c2 = ec;
    }
}

static bool is_cell_selected(int x, int y) {
    int r1, c1, r2, c2;
    sel_normalize(&r1, &c1, &r2, &c2);
    if (y < r1 || y > r2) return false;
    if (r1 == r2) return x >= c1 && x < c2;
    if (y == r1) return x >= c1;
    if (y == r2) return x < c2;
    return true;  // full row between start and end
}

// Encode a Unicode codepoint to UTF-8, return bytes written.
static int u32_to_utf8(uint32_t u, char *out) {
    if (u < 0x80) {
        out[0] = (char)u;
        return 1;
    } else if (u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    } else if (u < 0x10000) {
        out[0] = (char)(0xE0 | (u >> 12));
        out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[2] = (char)(0x80 | (u & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (u >> 18));
        out[1] = (char)(0x80 | ((u >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[3] = (char)(0x80 | (u & 0x3F));
        return 4;
    }
}

static void copy_selection_to_clipboard(void) {
    if (!g_tc || !g_has_selection) return;

    int r1, c1, r2, c2;
    sel_normalize(&r1, &c1, &r2, &c2);

    int cols = ttcore_get_cols(g_tc);
    int lines = ttcore_get_lines(g_tc);
    int nrows = r2 - r1 + 1;
    if (nrows <= 0) return;

    // Worst case: 4 bytes/char * cols + newline per row + NUL
    size_t buf_size = (size_t)(cols * 4 + 1) * (size_t)nrows + 1;
    char *buf = malloc(buf_size);
    if (!buf) return;

    size_t pos = 0;
    for (int y = r1; y <= r2; y++) {
        if (y < 0 || y >= lines) continue;

        int xstart = (y == r1) ? c1 : 0;
        int xend = (y == r2) ? c2 : cols;
        if (xstart < 0) xstart = 0;
        if (xend > cols) xend = cols;

        // Write cells for this row
        size_t row_start = pos;
        for (int x = xstart; x < xend; x++) {
            const buff_char_t *cell =
                (const buff_char_t *)ttcore_get_cell(g_tc, x, y);
            if (!cell) continue;
            uint32_t u = cell->u32;
            if (u == 0) u = ' ';
            pos += (size_t)u32_to_utf8(u, buf + pos);
        }

        // Trim trailing spaces from this row
        while (pos > row_start && buf[pos - 1] == ' ') pos--;

        // Add newline between rows (not after last)
        if (y < r2) buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, buf, (gint)pos);

    // Also set PRIMARY selection (X11 middle-click paste)
    GtkClipboard *primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    gtk_clipboard_set_text(primary, buf, (gint)pos);

    free(buf);
}

// Word boundary detection for double-click selection.
static bool is_word_char(uint32_t u) {
    if (u == 0 || u == ' ') return false;
    if (u < 128) {
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
            || (u >= '0' && u <= '9') || u == '_' || u == '-' || u == '.';
    }
    return iswalnum((wint_t)u);
}

// ---------------------------------------------------------------------------
// Font metrics
// ---------------------------------------------------------------------------

static void update_font_metrics(GtkWidget *widget) {
    PangoContext *pctx = gtk_widget_get_pango_context(widget);
    PangoFontMetrics *metrics = pango_context_get_metrics(
        pctx, g_font_desc, NULL);
    g_cell_width = pango_font_metrics_get_approximate_digit_width(metrics)
                   / PANGO_SCALE;
    g_font_ascent = pango_font_metrics_get_ascent(metrics) / PANGO_SCALE;
    int descent = pango_font_metrics_get_descent(metrics) / PANGO_SCALE;
    g_cell_height = g_font_ascent + descent;
    pango_font_metrics_unref(metrics);

    if (g_cell_width < 1) g_cell_width = 8;
    if (g_cell_height < 1) g_cell_height = 16;
}

// ---------------------------------------------------------------------------
// Draw callback
// ---------------------------------------------------------------------------

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data;
    if (!g_tc) return FALSE;

    if (g_cell_width == 0) update_font_metrics(widget);

    const ViewColors *vc = view_settings_get_colors();
    int cols = ttcore_get_cols(g_tc);
    int lines = ttcore_get_lines(g_tc);

    // Fill entire area with default bg
    cairo_set_source_rgba(cr, vc->bg.red, vc->bg.green,
                          vc->bg.blue, vc->bg.alpha);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *bold_desc = pango_font_description_copy(g_font_desc);
    pango_font_description_set_weight(bold_desc, PANGO_WEIGHT_BOLD);

    int cx, cy;
    ttcore_get_cursor_pos(g_tc, &cx, &cy);

    for (int y = 0; y < lines; y++) {
        for (int x = 0; x < cols; x++) {
            const buff_char_t *cell =
                (const buff_char_t *)ttcore_get_cell(g_tc, x, y);
            if (!cell) continue;

            // Determine fg/bg colors
            GdkRGBA fg_color, bg_color;
            if (cell->attr2 & ATTR2_BACK) {
                palette_to_rgba(cell->bg, &bg_color);
            } else {
                bg_color = vc->bg;
            }
            if (cell->attr2 & ATTR2_FORE) {
                palette_to_rgba(cell->fg, &fg_color);
            } else {
                fg_color = vc->fg;
            }

            // Reverse video
            if (cell->attr & ATTR_REVERSE) {
                GdkRGBA tmp = fg_color;
                fg_color = bg_color;
                bg_color = tmp;
            }

            // Selection highlight (G10b): invert fg/bg for selected cells
            if (g_has_selection && is_cell_selected(x, y)) {
                GdkRGBA tmp = fg_color;
                fg_color = bg_color;
                bg_color = tmp;
            }

            // Cursor rendering (only in live view)
            bool is_cursor = (g_scroll_offset == 0 && x == cx && y == cy);
            bool draw_underline_cursor = false;
            bool draw_hollow_cursor = false;

            if (is_cursor) {
                if (!g_has_focus) {
                    // Unfocused: hollow block outline, no blink
                    draw_hollow_cursor = true;
                } else if (g_cursor_blink && !g_cursor_visible) {
                    // Blink off phase: draw cell normally
                } else if (cursor_is_underline(g_cursor_shape)) {
                    // Underline cursor: draw cell normally, then underline bar
                    draw_underline_cursor = true;
                } else {
                    // Block cursor: invert fg/bg
                    GdkRGBA tmp = fg_color;
                    fg_color = bg_color;
                    bg_color = tmp;
                }
            }

            double px = x * g_cell_width;
            double py = y * g_cell_height;

            // Background rect
            cairo_set_source_rgba(cr, bg_color.red, bg_color.green,
                                  bg_color.blue, bg_color.alpha);
            cairo_rectangle(cr, px, py, g_cell_width, g_cell_height);
            cairo_fill(cr);

            // Character
            uint32_t u32 = cell->u32;
            if (u32 != 0 && u32 != ' ') {
                char utf8[8];
                int len = 0;
                if (u32 < 0x80) {
                    utf8[0] = (char)u32;
                    len = 1;
                } else if (u32 < 0x800) {
                    utf8[0] = (char)(0xC0 | (u32 >> 6));
                    utf8[1] = (char)(0x80 | (u32 & 0x3F));
                    len = 2;
                } else if (u32 < 0x10000) {
                    utf8[0] = (char)(0xE0 | (u32 >> 12));
                    utf8[1] = (char)(0x80 | ((u32 >> 6) & 0x3F));
                    utf8[2] = (char)(0x80 | (u32 & 0x3F));
                    len = 3;
                } else {
                    utf8[0] = (char)(0xF0 | (u32 >> 18));
                    utf8[1] = (char)(0x80 | ((u32 >> 12) & 0x3F));
                    utf8[2] = (char)(0x80 | ((u32 >> 6) & 0x3F));
                    utf8[3] = (char)(0x80 | (u32 & 0x3F));
                    len = 4;
                }

                // Font: bold or regular
                if (cell->attr & ATTR_BOLD) {
                    pango_layout_set_font_description(layout, bold_desc);
                } else {
                    pango_layout_set_font_description(layout, g_font_desc);
                }

                // Underline (SGR attribute, not cursor underline)
                PangoAttrList *attrs = NULL;
                if (cell->attr & ATTR_UNDER) {
                    attrs = pango_attr_list_new();
                    pango_attr_list_insert(attrs,
                        pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
                    pango_layout_set_attributes(layout, attrs);
                } else {
                    pango_layout_set_attributes(layout, NULL);
                }

                pango_layout_set_text(layout, utf8, len);

                cairo_set_source_rgba(cr, fg_color.red, fg_color.green,
                                      fg_color.blue, fg_color.alpha);
                cairo_move_to(cr, px, py);
                pango_cairo_show_layout(cr, layout);

                if (attrs) pango_attr_list_unref(attrs);
            }

            // Underline cursor bar (drawn after character)
            if (draw_underline_cursor) {
                cairo_set_source_rgba(cr, fg_color.red, fg_color.green,
                                      fg_color.blue, fg_color.alpha);
                cairo_rectangle(cr, px, py + g_cell_height - 2,
                                g_cell_width, 2);
                cairo_fill(cr);
            }

            // Hollow block cursor (unfocused)
            if (draw_hollow_cursor) {
                cairo_set_source_rgba(cr, fg_color.red, fg_color.green,
                                      fg_color.blue, fg_color.alpha);
                cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, px + 0.5, py + 0.5,
                                g_cell_width - 1.0, g_cell_height - 1.0);
                cairo_stroke(cr);
            }
        }
    }

    pango_font_description_free(bold_desc);
    g_object_unref(layout);

    // Update status bar cursor position
    status_bar_update_cursor(cx, cy);

    // Sync scrollbar adjustment (block signal to prevent feedback loop)
    if (g_scroll_adj && g_adj_changed_id) {
        int scroll_max = ttcore_get_scroll_max(g_tc);
        double upper = (double)(scroll_max + lines);
        double value = (double)(scroll_max - g_scroll_offset);
        g_signal_handler_block(g_scroll_adj, g_adj_changed_id);
        gtk_adjustment_configure(g_scroll_adj,
            value, 0.0, upper, 1.0, (double)lines, (double)lines);
        g_signal_handler_unblock(g_scroll_adj, g_adj_changed_id);
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Resize handler
// ---------------------------------------------------------------------------

// Debounced resize callback — fires 50ms after the last configure-event.
// Mirrors TeraTerm's DontChangeSize approach: don't resize during transitions.
static gboolean on_resize_timer(gpointer user_data) {
    (void)user_data;
    g_resize_timer_id = 0;

    if (!g_tc) return G_SOURCE_REMOVE;

    int new_cols = g_deferred_cols;
    int new_lines = g_deferred_lines;

    if (new_cols != g_cur_cols || new_lines != g_cur_lines) {
        if (g_resize_suppressed) {
            g_pending_cols = new_cols;
            g_pending_lines = new_lines;
            g_has_pending_resize = true;
        } else {
            g_cur_cols = new_cols;
            g_cur_lines = new_lines;
            ttcore_resize(g_tc, new_cols, new_lines);
            // Resize resets win_org_y to 0 — sync GUI scroll state.
            g_scroll_offset = 0;
            status_bar_update_geometry(new_cols, new_lines);
        }
        if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *event,
                              gpointer user_data) {
    (void)user_data;
    if (!g_tc) return FALSE;

    // Compute cell dimensions on first configure (widget is realized by now).
    if (g_cell_width == 0) update_font_metrics(widget);
    if (g_cell_width == 0) return FALSE;

    int new_cols = event->width / g_cell_width;
    int new_lines = event->height / g_cell_height;
    if (new_cols < 1) new_cols = 1;
    if (new_lines < 1) new_lines = 1;

    // Store desired size; restart debounce timer.
    g_deferred_cols = new_cols;
    g_deferred_lines = new_lines;

    if (g_resize_timer_id) g_source_remove(g_resize_timer_id);
    g_resize_timer_id = g_timeout_add(50, on_resize_timer, NULL);

    return FALSE;
}

// ---------------------------------------------------------------------------
// Key press handler — sends keystrokes to serial connection
// ---------------------------------------------------------------------------

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                              gpointer user_data) {
    (void)widget;
    (void)user_data;

    // Scrollback: PageUp/PageDown
    if (event->keyval == GDK_KEY_Page_Up) {
        if (g_tc) {
            g_scroll_offset += g_cur_lines;
            int max = ttcore_get_scroll_max(g_tc);
            if (g_scroll_offset > max) g_scroll_offset = max;
            ttcore_scroll_to(g_tc, g_scroll_offset);
            gtk_widget_queue_draw(g_drawing_area);
        }
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Page_Down) {
        if (g_tc) {
            g_scroll_offset -= g_cur_lines;
            if (g_scroll_offset < 0) g_scroll_offset = 0;
            ttcore_scroll_to(g_tc, g_scroll_offset);
            gtk_widget_queue_draw(g_drawing_area);
        }
        return TRUE;
    }

    // Ctrl+Shift+C: copy selection to clipboard (G10b)
    if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK)
        && event->keyval == GDK_KEY_C) {
        if (g_has_selection) copy_selection_to_clipboard();
        return TRUE;
    }

    // Cancel selection on any other keyboard input (G10b)
    if (g_has_selection) {
        g_has_selection = false;
        g_selecting = false;
        if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    }

    // Consume all keys even when disconnected (no propagation to toolbar).
    // Just don't send anything if the connection is closed.
    if (!serial_conn_is_open()) return TRUE;

    // Snap to live view when typing
    if (g_scroll_offset != 0 && g_tc) {
        g_scroll_offset = 0;
        ttcore_scroll_to(g_tc, 0);
    }

    // Map special keys to VT sequences
    const char *seq = NULL;
    switch (event->keyval) {
        case GDK_KEY_Return:    seq = "\r"; break;
        case GDK_KEY_BackSpace: seq = g_backspace_del ? "\x7f" : "\x08"; break;
        case GDK_KEY_Tab:       seq = "\t"; break;
        case GDK_KEY_Escape:    seq = "\x1b"; break;
        case GDK_KEY_Up:        seq = "\x1b[A"; break;
        case GDK_KEY_Down:      seq = "\x1b[B"; break;
        case GDK_KEY_Right:     seq = "\x1b[C"; break;
        case GDK_KEY_Left:      seq = "\x1b[D"; break;
        case GDK_KEY_Home:      seq = "\x1b[H"; break;
        case GDK_KEY_End:       seq = "\x1b[F"; break;
        case GDK_KEY_Insert:    seq = "\x1b[2~"; break;
        case GDK_KEY_Delete:    seq = "\x1b[3~"; break;
        case GDK_KEY_F1:        seq = "\x1bOP"; break;
        case GDK_KEY_F2:        seq = "\x1bOQ"; break;
        case GDK_KEY_F3:        seq = "\x1bOR"; break;
        case GDK_KEY_F4:        seq = "\x1bOS"; break;
        case GDK_KEY_F5:        seq = "\x1b[15~"; break;
        case GDK_KEY_F6:        seq = "\x1b[17~"; break;
        case GDK_KEY_F7:        seq = "\x1b[18~"; break;
        case GDK_KEY_F8:        seq = "\x1b[19~"; break;
        case GDK_KEY_F9:        seq = "\x1b[20~"; break;
        case GDK_KEY_F10:       seq = "\x1b[21~"; break;
        case GDK_KEY_F11:       seq = "\x1b[23~"; break;
        case GDK_KEY_F12:       seq = "\x1b[24~"; break;
        default: break;
    }

    if (seq) {
        serial_conn_write((const uint8_t *)seq, (int)strlen(seq));
        return TRUE;
    }

    // Ctrl+letter
    if ((event->state & GDK_CONTROL_MASK) && event->keyval >= GDK_KEY_a
        && event->keyval <= GDK_KEY_z) {
        uint8_t ch = (uint8_t)(event->keyval - GDK_KEY_a + 1);
        serial_conn_write(&ch, 1);
        return TRUE;
    }

    // Regular character (from event->string)
    if (event->string && event->string[0] && !(event->state & GDK_CONTROL_MASK)
        && !(event->state & GDK_MOD1_MASK)) {
        int len = (int)strlen(event->string);
        if (len > 0) {
            serial_conn_write((const uint8_t *)event->string, len);
            return TRUE;
        }
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Mouse scroll handler — scrollback navigation
// ---------------------------------------------------------------------------

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event,
                           gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (!g_tc) return FALSE;

    int delta = 3;
    if (event->direction == GDK_SCROLL_UP) {
        g_scroll_offset += delta;
        int max = ttcore_get_scroll_max(g_tc);
        if (g_scroll_offset > max) g_scroll_offset = max;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        g_scroll_offset -= delta;
        if (g_scroll_offset < 0) g_scroll_offset = 0;
    } else {
        return FALSE;
    }

    ttcore_scroll_to(g_tc, g_scroll_offset);
    gtk_widget_queue_draw(g_drawing_area);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Mouse selection handlers (G10b)
// ---------------------------------------------------------------------------

static void pixel_to_cell(double px, double py, int *col, int *row) {
    int cols = g_tc ? ttcore_get_cols(g_tc) : g_cur_cols;
    int lines = g_tc ? ttcore_get_lines(g_tc) : g_cur_lines;
    int c = (g_cell_width > 0) ? (int)(px / g_cell_width) : 0;
    int r = (g_cell_height > 0) ? (int)(py / g_cell_height) : 0;
    if (c < 0) c = 0;
    if (c >= cols) c = cols;  // allow cols (one past last) for end-of-line
    if (r < 0) r = 0;
    if (r >= lines) r = lines - 1;
    *col = c;
    *row = r;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                 gpointer user_data) {
    (void)user_data;
    gtk_widget_grab_focus(widget);

    if (event->button != 1) return FALSE;

    int col, row;
    pixel_to_cell(event->x, event->y, &col, &row);

    // Double-click: select word
    if (event->type == GDK_2BUTTON_PRESS && g_tc) {
        int cols = ttcore_get_cols(g_tc);
        // Find word boundaries at (col, row)
        int wstart = col, wend = col;

        // Scan left
        while (wstart > 0) {
            const buff_char_t *c =
                (const buff_char_t *)ttcore_get_cell(g_tc, wstart - 1, row);
            if (!c || !is_word_char(c->u32)) break;
            wstart--;
        }
        // Scan right
        while (wend < cols) {
            const buff_char_t *c =
                (const buff_char_t *)ttcore_get_cell(g_tc, wend, row);
            if (!c || !is_word_char(c->u32)) break;
            wend++;
        }

        if (wend > wstart) {
            g_sel_start_col = wstart;
            g_sel_start_row = row;
            g_sel_end_col = wend;
            g_sel_end_row = row;
            g_has_selection = true;
            g_selecting = false;
            copy_selection_to_clipboard();
            gtk_widget_queue_draw(g_drawing_area);
        }
        return TRUE;
    }

    // Single click: start selection
    g_selecting = true;
    g_has_selection = false;
    g_sel_start_col = col;
    g_sel_start_row = row;
    g_sel_end_col = col;
    g_sel_end_row = row;
    gtk_widget_queue_draw(g_drawing_area);

    return TRUE;
}

static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                  gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (!g_selecting) return FALSE;

    int col, row;
    pixel_to_cell(event->x, event->y, &col, &row);

    g_sel_end_col = col;
    g_sel_end_row = row;
    g_has_selection = true;
    gtk_widget_queue_draw(g_drawing_area);

    return TRUE;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                   gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (event->button != 1 || !g_selecting) return FALSE;

    g_selecting = false;

    // Click without drag: cancel selection
    if (g_sel_start_col == g_sel_end_col
        && g_sel_start_row == g_sel_end_row) {
        g_has_selection = false;
        gtk_widget_queue_draw(g_drawing_area);
        return TRUE;
    }

    // Auto-copy to clipboard (xterm-style)
    if (g_has_selection) {
        copy_selection_to_clipboard();
    }

    gtk_widget_queue_draw(g_drawing_area);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Scrollbar value-changed handler
// ---------------------------------------------------------------------------

static void on_scroll_adj_changed(GtkAdjustment *adj, gpointer user_data) {
    (void)user_data;
    if (!g_tc || g_adj_updating) return;

    int scroll_max = ttcore_get_scroll_max(g_tc);
    double value = gtk_adjustment_get_value(adj);
    g_scroll_offset = scroll_max - (int)value;
    if (g_scroll_offset < 0) g_scroll_offset = 0;
    if (g_scroll_offset > scroll_max) g_scroll_offset = scroll_max;
    ttcore_scroll_to(g_tc, g_scroll_offset);
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

GtkWidget *terminal_view_create(void) {
    // Font from view_settings
    g_font_desc = pango_font_description_from_string(
        view_settings_get_font());

    // libttcore callbacks.
    // The GUI uses a poll-driven rendering model: terminal_view_feed_data()
    // calls ttcore_parse_data() then queue_draw() for a full redraw.
    // Therefore update_rect, scroll_screen, clear_screen are intentionally
    // NULL — we don't use incremental invalidation callbacks.
    // Only set_cursor_style is needed to receive DECSCUSR from the remote.
    ttcore_callbacks_t cb = {0};
    cb.set_cursor_style = on_cursor_style_cb;

    g_tc = ttcore_create_ex(80, 24, 500, &cb);
    g_cur_cols = 80;
    g_cur_lines = 24;

    // Drawing area
    g_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(g_drawing_area, TRUE);
    gtk_widget_set_size_request(g_drawing_area, 640, 384);
    gtk_widget_add_events(g_drawing_area,
        GDK_KEY_PRESS_MASK | GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK
        | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK
        | GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(g_drawing_area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(g_drawing_area, "configure-event",
                     G_CALLBACK(on_configure), NULL);
    g_signal_connect(g_drawing_area, "key-press-event",
                     G_CALLBACK(on_key_press), NULL);
    g_signal_connect(g_drawing_area, "scroll-event",
                     G_CALLBACK(on_scroll), NULL);
    g_signal_connect(g_drawing_area, "button-press-event",
                     G_CALLBACK(on_button_press), NULL);
    g_signal_connect(g_drawing_area, "button-release-event",
                     G_CALLBACK(on_button_release), NULL);
    g_signal_connect(g_drawing_area, "motion-notify-event",
                     G_CALLBACK(on_motion_notify), NULL);
    g_signal_connect(g_drawing_area, "focus-in-event",
                     G_CALLBACK(on_focus_in), NULL);
    g_signal_connect(g_drawing_area, "focus-out-event",
                     G_CALLBACK(on_focus_out), NULL);

    // Scrollbar (vertical) — inverted mapping: GTK value 0 = oldest (top)
    g_scroll_adj = gtk_adjustment_new(0.0, 0.0, 1.0, 1.0, 24.0, 24.0);
    g_scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, g_scroll_adj);
    g_adj_changed_id = g_signal_connect(g_scroll_adj, "value-changed",
                                         G_CALLBACK(on_scroll_adj_changed),
                                         NULL);

    // HBox: drawing area (expand) + scrollbar (fixed)
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), g_drawing_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), g_scrollbar, FALSE, FALSE, 0);

    return hbox;
}

void terminal_view_destroy(void) {
    if (g_resize_timer_id) {
        g_source_remove(g_resize_timer_id);
        g_resize_timer_id = 0;
    }
    if (g_blink_timer_id) {
        g_source_remove(g_blink_timer_id);
        g_blink_timer_id = 0;
    }
    if (g_scroll_adj && g_adj_changed_id) {
        g_signal_handler_disconnect(g_scroll_adj, g_adj_changed_id);
        g_adj_changed_id = 0;
    }
    g_scroll_adj = NULL;
    g_scrollbar = NULL;
    if (g_tc) {
        ttcore_destroy(g_tc);
        g_tc = NULL;
    }
    if (g_font_desc) {
        pango_font_description_free(g_font_desc);
        g_font_desc = NULL;
    }
}

void terminal_view_feed_data(const uint8_t *data, int len) {
    if (!g_tc || !data || len <= 0) return;
    ttcore_parse_data(g_tc, data, (size_t)len);

    // Sync scroll offset after parse (WinOrgY model)
    g_scroll_offset = ttcore_get_scroll_pos(g_tc);

    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
}

void terminal_view_grab_focus(void) {
    if (g_drawing_area) gtk_widget_grab_focus(g_drawing_area);
}

void terminal_view_queue_draw(void) {
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
}

void terminal_view_suppress_resize(bool suppress) {
    g_resize_suppressed = suppress;
    if (suppress) {
        g_has_pending_resize = false;
    }
}

void terminal_view_apply_pending_resize(void) {
    if (!g_tc || !g_has_pending_resize) return;
    g_has_pending_resize = false;

    if (g_pending_cols != g_cur_cols || g_pending_lines != g_cur_lines) {
        g_cur_cols = g_pending_cols;
        g_cur_lines = g_pending_lines;
        ttcore_resize(g_tc, g_cur_cols, g_cur_lines);
        g_scroll_offset = 0;
        status_bar_update_geometry(g_cur_cols, g_cur_lines);
        if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
    }
}

void terminal_view_apply_settings(void) {
    if (!g_tc || !g_drawing_area) return;

    // Update font from view_settings
    PangoFontDescription *new_desc = pango_font_description_from_string(
        view_settings_get_font());
    if (g_font_desc) pango_font_description_free(g_font_desc);
    g_font_desc = new_desc;

    // Recalculate cell dimensions
    update_font_metrics(g_drawing_area);

    // Recalculate cols/lines from current widget size
    GtkAllocation alloc;
    gtk_widget_get_allocation(g_drawing_area, &alloc);
    int new_cols = alloc.width / g_cell_width;
    int new_lines = alloc.height / g_cell_height;
    if (new_cols < 1) new_cols = 1;
    if (new_lines < 1) new_lines = 1;

    if (new_cols != g_cur_cols || new_lines != g_cur_lines) {
        g_cur_cols = new_cols;
        g_cur_lines = new_lines;
        ttcore_resize(g_tc, new_cols, new_lines);
        g_scroll_offset = 0;
        status_bar_update_geometry(new_cols, new_lines);
    }

    gtk_widget_queue_draw(g_drawing_area);
}

void terminal_view_set_cursor_style(int shape, bool blink) {
    g_cursor_shape = shape;
    g_cursor_blink = blink;
    update_blink_timer();
    if (g_drawing_area) gtk_widget_queue_draw(g_drawing_area);
}

void terminal_view_set_backspace_del(bool use_del) {
    g_backspace_del = use_del;
}

bool terminal_view_copy_selection(void) {
    if (!g_has_selection) return false;
    copy_selection_to_clipboard();
    return true;
}

bool terminal_view_has_selection(void) {
    return g_has_selection;
}

void terminal_view_clear_screen(void) {
    if (!g_tc) return;
    // ESC[2J = erase display (all), ESC[H = cursor home (1,1).
    static const uint8_t seq[] = "\x1b[2J\x1b[H";
    ttcore_parse_data(g_tc, seq, sizeof(seq) - 1);
    g_scroll_offset = 0;
    if (g_drawing_area) {
        gtk_widget_queue_draw(g_drawing_area);
    }
}
