// 2025-01-28 10:00 v0.1.0
// vtparse.c — Headless VT100/VT220/xterm state-machine parser
// Ported from TeraTerm 5 vtterm.c — Win32-free, C99.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2025 ttcore-port contributors — BSD 3-Clause

/* POSIX.1-2008 required for clock_gettime / struct timespec */
#define _POSIX_C_SOURCE 200809L

#include "vtparse.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>
#include <time.h>    /* clock_gettime, struct timespec */

// ---------------------------------------------------------------------------
// C0 / C1 control character defines (matching original vtterm.c)
// ---------------------------------------------------------------------------
#define ENQ  0x05
#define BEL  0x07
#define BS   0x08
#define HT   0x09
#define LF   0x0A
#define VT   0x0B
#define FF   0x0C
#define CR   0x0D
#define SO   0x0E
#define SI   0x0F
#define DLE  0x10
#define CAN  0x18
#define SUB  0x1A
#define ESC  0x1B
#define FS   0x1C
#define GS   0x1D
#define RS   0x1E
#define US   0x1F

/* C1 (8-bit) */
#define IND  0x84
#define NEL  0x85
#define HTS  0x88
#define RI   0x8D
#define SS2  0x8E
#define SS3  0x8F
#define DCS  0x90
#define SOS  0x98
#define DECID 0x9A
#define CSI  0x9B
#define ST   0x9C
#define OSC  0x9D
#define PM   0x9E
#define APC  0x9F

// ---------------------------------------------------------------------------
// Parse modes (internal)
// ---------------------------------------------------------------------------
#define kModeFirst    0
#define kModeESC      1
#define kModeDCS      2
#define kModeDCUserKey 3
#define kModeSOS      4
#define kModeCSI      5
#define kModeXS       6   /* OSC / APC / PM */
#define kModeDLE      7
#define kModeCAN      8
#define kModeIgnore   9

// ---------------------------------------------------------------------------
// Internal per-instance state
// ---------------------------------------------------------------------------
struct VtParser {
    VtParserConfig   cfg;
    VtParserCallbacks cbs;
    void            *user_data;

    /* --- Parser state --- */
    int     parse_mode;
    bool    esc_flag;
    bool    just_after_esc;
    int     change_emu;    /* IdTEK if TEK switch requested */

    /* CSI parameter accumulation */
    int     param[VTPARSER_NPARAMS_MAX + 1];
    int     sub_param[VTPARSER_NPARAMS_MAX + 1][VTPARSER_NSPARAMS_MAX + 1];
    int     n_param;
    int     n_sub_param[VTPARSER_NPARAMS_MAX + 1];
    bool    first_param;
    uint8_t int_char[VTPARSER_INTCHARS_MAX + 1];
    int     int_count;
    uint8_t prv;           /* private leader: <, =, >, ? */

    /* OSC / DCS / XS string buffer */
    char    osc_buf[VTPARSER_OSC_BUF_MAX + 1];
    int     osc_len;
    bool    osc_esc_flag;  /* ST via ESC \ */

    /* Terminal modes */
    bool    insert_mode;
    bool    lf_mode;          /* true = LF also sends CR */
    bool    auto_wrap_mode;
    bool    clear_then_home;
    bool    relative_org_mode;
    bool    focus_report_mode;
    bool    alt_scr;
    bool    lr_margin_mode;
    bool    rectangle_mode;
    bool    bracketed_paste;
    bool    accept_wheel_to_cursor;
    bool    auto_repeat_mode;
    bool    accept_8bit_ctrl;

    /* VT emulation level */
    int     vt_level;         /* 1=VT100, 2=VT220 … */

    /* Keypad / cursor key modes */
    bool    appli_key_mode;
    bool    appli_cursor_mode;
    bool    appli_escape_mode;

    /* Mouse tracking */
    int     mouse_report_mode;
    int     mouse_report_ext_mode;
    int     last_x, last_y;
    int     button_stat;

    /* Printer */
    bool    printer_mode;
    bool    auto_print_mode;
    bool    print_ex;         /* print extent: true=screen false=region */

    /* Status line */
    bool    status_wrap;
    bool    status_cursor;
    bool    main_wrap;
    bool    main_cursor;

    /* Previous character tracking */
    uint32_t last_put_character;
    uint8_t  prev_character;
    bool     prev_cr_lf_generated_crlf;

    /* User-defined key definition state */
    bool    wait_key_id;
    bool    wait_hi;
    int     new_key_id;
    int     new_key_len;
    uint8_t new_key_str[VTPARSER_FUNC_KEY_STR_MAX];

    /* Current character attribute */
    VtCharAttr char_attr;

    /* Saved cursor slots (main / status / alt-screen) */
    VtSavedCursor saved_cursor_main;
    VtSavedCursor saved_cursor_status;
    VtSavedCursor saved_cursor_alt;

    /* Beep rate-limiting */
    uint64_t beep_start_ms;
    uint64_t beep_suppress_ms;
    uint32_t beep_over_used_count;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Monotonic millisecond clock — replaces GetTickCount() */
static uint64_t MonoMs(void) {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (uint64_t)tp.tv_sec * 1000u + (uint64_t)(tp.tv_nsec / 1000000);
}

/* Safe write_output — no-op if callback not set */
static void WriteOutput(VtParser *p, const uint8_t *data, size_t len) {
    if (p->cbs.write_output) {
        p->cbs.write_output(p, data, len);
    }
}

static void WriteStr(VtParser *p, const char *s) {
    WriteOutput(p, (const uint8_t *)s, strlen(s));
}

/* Accept8BitCtrl: replaces the macro from vtterm.c */
static bool Accept8BitCtrl(const VtParser *p) {
    return (p->vt_level >= 2) && p->accept_8bit_ctrl;
}

/* Default VtCharAttr */
static VtCharAttr DefaultCharAttr(void) {
    VtCharAttr a;
    memset(&a, 0, sizeof(a));
    a.fg = VTPARSER_COLOR_DEFAULT;
    a.bg = VTPARSER_COLOR_DEFAULT;
    a.underline_color = VTPARSER_COLOR_DEFAULT;
    return a;
}

// ---------------------------------------------------------------------------
// Parameter helpers (mirrors vtterm.c macros)
// ---------------------------------------------------------------------------

static void ClearParams(VtParser *p) {
    p->int_count = 0;
    p->n_param = 1;
    p->n_sub_param[1] = 0;
    p->param[1] = 0;
    p->prv = 0;
}

/* Check: if p==0 set to 1; if p>max or <0 set to max */
#define CheckParamVal(val, max_val)                      \
    do {                                                  \
        if ((val) == 0)          { (val) = 1; }          \
        else if ((val) > (max_val) || (val) < 0)         \
                                 { (val) = (max_val); }   \
    } while (0)

/* Check: if p>max or <=0 set to max */
#define CheckParamValMax(val, max_val)                    \
    do {                                                  \
        if ((val) > (max_val) || (val) <= 0)              \
            (val) = (max_val);                            \
    } while (0)

/* Ensure at least n params exist, filling missing ones with 0 */
static void RequiredParams(VtParser *p, int n) {
    if (n > 1) {
        while (p->n_param < n) {
            p->n_param++;
            p->param[p->n_param] = 0;
            p->n_sub_param[p->n_param] = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Callback dispatch wrappers — all effect-firing goes here
// ---------------------------------------------------------------------------

static void CB_PutChar(VtParser *p, uint32_t codepoint) {
    p->last_put_character = codepoint;
    if (p->cbs.put_char) {
        p->cbs.put_char(p, codepoint, &p->char_attr);
    }
}

static void CB_CursorMove(VtParser *p, int x, int y) {
    if (p->cbs.cursor_move) {
        p->cbs.cursor_move(p, x, y);
    }
}

static void CB_CursorUp(VtParser *p, int n, bool affect_margin) {
    if (p->cbs.cursor_up) p->cbs.cursor_up(p, n, affect_margin);
}

static void CB_CursorDown(VtParser *p, int n, bool affect_margin) {
    if (p->cbs.cursor_down) p->cbs.cursor_down(p, n, affect_margin);
}

static void CB_CursorRight(VtParser *p, int n, bool affect_margin) {
    if (p->cbs.cursor_right) p->cbs.cursor_right(p, n, affect_margin);
}

static void CB_CursorLeft(VtParser *p, int n, bool affect_margin) {
    if (p->cbs.cursor_left) p->cbs.cursor_left(p, n, affect_margin);
}

static void CB_SetCharAttr(VtParser *p) {
    if (p->cbs.set_char_attr) {
        p->cbs.set_char_attr(p, &p->char_attr);
    }
}

static void CB_Bell(VtParser *p, int type) {
    if (p->cbs.on_bell) p->cbs.on_bell(p, type);
}

static void CB_LineFeed(VtParser *p) {
    if (p->cbs.line_feed) p->cbs.line_feed(p);
}

static void CB_CarriageReturn(VtParser *p) {
    if (p->cbs.carriage_return) p->cbs.carriage_return(p);
}

static void CB_BackSpace(VtParser *p) {
    if (p->cbs.back_space) p->cbs.back_space(p);
}

static void CB_ReverseIndex(VtParser *p) {
    if (p->cbs.reverse_index) p->cbs.reverse_index(p);
}

static void CB_SetTabStop(VtParser *p) {
    if (p->cbs.set_tab_stop) p->cbs.set_tab_stop(p);
}

static void CB_UpdateScreen(VtParser *p) {
    if (p->cbs.update_screen) p->cbs.update_screen(p);
}

// ---------------------------------------------------------------------------
// Bell rate-limiting (replaces Win32 GetTickCount-based RingBell)
// ---------------------------------------------------------------------------

static void RingBell(VtParser *p, int type) {
    uint64_t now = MonoMs();
    uint32_t suppress_ms = p->cfg.beep_suppress_ms;
    uint32_t over_time_ms = p->cfg.beep_over_used_time_ms;

    if (now - p->beep_suppress_ms < suppress_ms) {
        p->beep_suppress_ms = now;
        return;
    }

    if (now - p->beep_start_ms < over_time_ms) {
        if (p->beep_over_used_count <= 1) {
            p->beep_suppress_ms = now;
            return;
        }
        p->beep_over_used_count--;
    } else {
        p->beep_start_ms = now;
        p->beep_over_used_count = p->cfg.beep_over_used_count;
    }

    CB_Bell(p, type);
}

// ---------------------------------------------------------------------------
// SendCSIstr helper — used by DSR responses and DECRQM
// ---------------------------------------------------------------------------

static void SendCSIstr(VtParser *p, const char *str, int len) {
    if (Accept8BitCtrl(p)) {
        /* 8-bit CSI = 0x9B */
        uint8_t csi_byte = 0x9B;
        WriteOutput(p, &csi_byte, 1);
    } else {
        WriteStr(p, "\033[");
    }
    if (len < 0) {
        len = (int)strlen(str);
    }
    WriteOutput(p, (const uint8_t *)str, (size_t)len);
}

// ---------------------------------------------------------------------------
// AnswerTerminalType — DA response
// ---------------------------------------------------------------------------

static void AnswerTerminalType(VtParser *p) {
    char buf[64];
    /* Primary DA: ESC [ ? Ps c */
    switch (p->vt_level) {
    case 1:
        snprintf(buf, sizeof(buf), "?1;2c");  /* VT100 with AVO */
        break;
    case 2:
        snprintf(buf, sizeof(buf), "?62;1;6;8;9;15;22c");  /* VT220 */
        break;
    case 3:
    case 4:
    case 5:
    default:
        snprintf(buf, sizeof(buf), "?63;1;6;8;9;15;22;29c");  /* VT420 */
        break;
    }
    SendCSIstr(p, buf, -1);
}

// ---------------------------------------------------------------------------
// Save / Restore cursor
// ---------------------------------------------------------------------------

static void SaveCursorToSlot(VtParser *p, VtSavedCursor *slot) {
    if (p->cbs.get_cursor_pos) {
        p->cbs.get_cursor_pos(p, &slot->cursor_x, &slot->cursor_y);
    }
    slot->attr = p->char_attr;
    slot->auto_wrap_mode = p->auto_wrap_mode;
    slot->relative_org_mode = p->relative_org_mode;
}

static void RestoreCursorFromSlot(VtParser *p, const VtSavedCursor *slot) {
    CB_CursorMove(p, slot->cursor_x, slot->cursor_y);
    p->char_attr = slot->attr;
    CB_SetCharAttr(p);
    p->auto_wrap_mode = slot->auto_wrap_mode;
    p->relative_org_mode = slot->relative_org_mode;
}

static void SaveCursor(VtParser *p) {
    VtSavedCursor *slot = p->alt_scr ? &p->saved_cursor_alt
                                     : &p->saved_cursor_main;
    SaveCursorToSlot(p, slot);
}

static void RestoreCursor(VtParser *p) {
    const VtSavedCursor *slot = p->alt_scr ? &p->saved_cursor_alt
                                           : &p->saved_cursor_main;
    RestoreCursorFromSlot(p, slot);
}

// ---------------------------------------------------------------------------
// SGR — Select Graphic Rendition
// ---------------------------------------------------------------------------

/* Parse SGR colour encoding for 38/48/58.
 * Handles two input formats:
 *   Colon sub-params  (38:5:N  or  38:2:R:G:B)  — *pn unchanged
 *   Semicolon params  (38;5;N  or  38;2;R;G;B)  — *pn advanced past extras
 *
 * n_param is 1-based (last valid index == n_param), so lookahead guard is
 * idx + N <= p->n_param.
 */
static uint32_t ParseSGRColor(VtParser *p, int *pn) {
    int idx = *pn;

    /* ── Colon sub-param path ──────────────────────────────────────────── */
    /* RGB24: sub_param[1]==2, sub_param[2..4] = R, G, B */
    if (p->n_sub_param[idx] >= 4 && p->sub_param[idx][1] == 2) {
        uint8_t r = (uint8_t)(p->sub_param[idx][2] & 0xFF);
        uint8_t g = (uint8_t)(p->sub_param[idx][3] & 0xFF);
        uint8_t b = (uint8_t)(p->sub_param[idx][4] & 0xFF);
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    /* 256-colour: sub_param[1]==5, sub_param[2] = index */
    if (p->n_sub_param[idx] >= 2 && p->sub_param[idx][1] == 5) {
        return 0x02000000u | (uint32_t)(p->sub_param[idx][2] & 0xFF);
    }

    /* ── Semicolon legacy path ─────────────────────────────────────────── */
    /* param[] is 1-based; n_param is the last valid index.
     * Guard: idx + N <= n_param ensures param[idx+N] is in bounds. */
    if (p->n_sub_param[idx] == 0 && idx + 1 <= p->n_param) {
        int mode = p->param[idx + 1];
        if (mode == 5 && idx + 2 <= p->n_param) {
            /* 38;5;N — consume mode + index */
            *pn += 2;
            return 0x02000000u | (uint32_t)(p->param[idx + 2] & 0xFF);
        }
        if (mode == 2 && idx + 4 <= p->n_param) {
            /* 38;2;R;G;B — consume mode + three colour components */
            uint8_t r = (uint8_t)(p->param[idx + 2] & 0xFF);
            uint8_t g = (uint8_t)(p->param[idx + 3] & 0xFF);
            uint8_t b = (uint8_t)(p->param[idx + 4] & 0xFF);
            *pn += 4;
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    return VTPARSER_COLOR_DEFAULT;
}

static void CSSetAttr(VtParser *p) {
    int i;
    if (p->n_param == 0 || (p->n_param == 1 && p->param[1] == 0)) {
        /* Reset all attributes */
        p->char_attr = DefaultCharAttr();
        CB_SetCharAttr(p);
        return;
    }

    for (i = 1; i <= p->n_param; i++) {
        int val = p->param[i];
        switch (val) {
        case 0:  p->char_attr = DefaultCharAttr(); break;
        case 1:  p->char_attr.bold          = true;  break;
        case 2:  /* faint/dim — map to bold=false */ break;
        case 3:  /* italic — not tracked */ break;
        case 4:
            /* Kitty underline style extension: 4:N sets style N, 4:0 clears */
            if (p->n_sub_param[i] >= 1) {
                int us = p->sub_param[i][1];
                if (us == 0) {
                    p->char_attr.underline = false;
                    p->char_attr.underline_style = 0;
                } else {
                    p->char_attr.underline = true;
                    p->char_attr.underline_style = (uint8_t)us;
                }
            } else {
                p->char_attr.underline = true;
                p->char_attr.underline_style = 1;
            }
            break;
        case 5:  /* blink slow */
        case 6:  p->char_attr.blink = true;  break;
        case 7:  p->char_attr.reverse   = true;  break;
        case 8:  p->char_attr.invisible  = true;  break;
        case 9:  p->char_attr.strikethrough = true; break;
        case 21:
            p->char_attr.double_underline = true;
            p->char_attr.underline_style = 2;
            break;
        case 22: p->char_attr.bold        = false; break;
        case 24:
            p->char_attr.underline = false;
            p->char_attr.underline_style = 0;
            p->char_attr.double_underline = false;
            break;
        case 25: p->char_attr.blink       = false; break;
        case 27: p->char_attr.reverse     = false; break;
        case 28: p->char_attr.invisible   = false; break;
        case 29: p->char_attr.strikethrough = false; break;
        case 38:
            p->char_attr.fg = ParseSGRColor(p, &i);
            break;
        case 39: p->char_attr.fg = VTPARSER_COLOR_DEFAULT; break;
        case 48:
            p->char_attr.bg = ParseSGRColor(p, &i);
            break;
        case 49: p->char_attr.bg = VTPARSER_COLOR_DEFAULT; break;
        case 53: p->char_attr.overline = true;  break;
        case 55: p->char_attr.overline = false; break;
        case 58:
            p->char_attr.underline_color = ParseSGRColor(p, &i);
            break;
        case 59:
            p->char_attr.underline_color = VTPARSER_COLOR_DEFAULT;
            break;
        default:
            /* 16-colour foreground: 30-37, 90-97 */
            if (val >= 30 && val <= 37) {
                p->char_attr.fg = 0x03000000u | (uint32_t)(val - 30);
            } else if (val >= 90 && val <= 97) {
                p->char_attr.fg = 0x03000000u | (uint32_t)(val - 90 + 8);
            }
            /* 16-colour background: 40-47, 100-107 */
            else if (val >= 40 && val <= 47) {
                p->char_attr.bg = 0x03000000u | (uint32_t)(val - 40);
            } else if (val >= 100 && val <= 107) {
                p->char_attr.bg = 0x03000000u | (uint32_t)(val - 100 + 8);
            }
            break;
        }
    }
    CB_SetCharAttr(p);
}

// ---------------------------------------------------------------------------
// DSR — Device Status Report
// ---------------------------------------------------------------------------

static void SendCursorPosReport(VtParser *p) {
    int x = 0, y = 0;
    char buf[32];
    if (p->cbs.get_cursor_pos) {
        p->cbs.get_cursor_pos(p, &x, &y);
    }
    snprintf(buf, sizeof(buf), "%d;%dR", y + 1, x + 1);
    SendCSIstr(p, buf, -1);
}

static void CS_n_Mode(VtParser *p) {
    switch (p->param[1]) {
    case 5: {   /* DSR — device status: always OK */
        char buf[] = "0n";
        SendCSIstr(p, buf, 2);
        break;
    }
    case 6:     /* CPR — cursor position report */
        SendCursorPosReport(p);
        break;
    default:
        break;
    }
}

static void CSQ_n_Mode(VtParser *p) {
    switch (p->param[1]) {
    case 6:     /* DECDSR cursor pos */
        SendCursorPosReport(p);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Mode set/reset helpers (SM/RM and DECSET/DECRST)
// ---------------------------------------------------------------------------

static void CS_h_Mode(VtParser *p) {   /* SM */
    switch (p->param[1]) {
    case 4:   p->insert_mode = true;  break;
    case 20:  p->lf_mode = true;      break;
    default:  break;
    }
}

static void CS_l_Mode(VtParser *p) {   /* RM */
    switch (p->param[1]) {
    case 4:   p->insert_mode = false; break;
    case 20:  p->lf_mode = false;     break;
    default:  break;
    }
}

/* Common DECSET/DECRST action — set if 'on', reset if !on */
static void ApplyDECMode(VtParser *p, int mode_num, bool on) {
    switch (mode_num) {
    case 1:    /* DECCKM — application cursor keys */
        if (!p->cfg.disable_app_cursor) {
            p->appli_cursor_mode = on;
            if (p->cbs.set_appli_cursor) {
                p->cbs.set_appli_cursor(p, on);
            }
        }
        break;
    case 3:    /* DECCOLM — 80/132 columns */
        if (p->cbs.on_resize) {
            p->cbs.on_resize(p, on ? 132 : 80, -1);
        }
        break;
    case 6:    /* DECOM — origin mode */
        p->relative_org_mode = on;
        CB_CursorMove(p, 0, 0);
        break;
    case 7:    /* DECAWM — auto-wrap mode */
        p->auto_wrap_mode = on;
        if (p->cbs.set_appli_cursor) { /* reuse — no, just ignore */ }
        break;
    case 8:    /* DECARM — auto-repeat */
        p->auto_repeat_mode = on;
        break;
    case 9:    /* XT_MSE_X10 */
        if (on) {
            p->mouse_report_mode = VTPARSER_MOUSE_X10;
        } else {
            p->mouse_report_mode = VTPARSER_MOUSE_NONE;
        }
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 19:   /* DECPEX — print extent */
        p->print_ex = on;
        break;
    case 25:   /* DECTCEM — cursor visible */
        if (p->cbs.cursor_set_style) {
            p->cbs.cursor_set_style(p, -1 /* keep style */, !on ? 0 : 1);
        }
        break;
    case 47:   /* XT_ALTSCRN */
    case 1047:
        if (p->cbs.switch_screen) {
            p->cbs.switch_screen(p, on);
        }
        p->alt_scr = on;
        break;
    case 1000: /* XT_MSE_X11 */
        p->mouse_report_mode =
            on ? VTPARSER_MOUSE_NORMAL : VTPARSER_MOUSE_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1002: /* XT_MSE_BTN */
        p->mouse_report_mode =
            on ? VTPARSER_MOUSE_BTN_EVT : VTPARSER_MOUSE_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1003: /* XT_MSE_ANY */
        p->mouse_report_mode =
            on ? VTPARSER_MOUSE_ANY_EVT : VTPARSER_MOUSE_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1004: /* focus tracking */
        p->focus_report_mode = on;
        break;
    case 1005: /* UTF-8 mouse ext */
        p->mouse_report_ext_mode =
            on ? VTPARSER_MOUSE_EXT_UTF8 : VTPARSER_MOUSE_EXT_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1006: /* SGR mouse ext */
        p->mouse_report_ext_mode =
            on ? VTPARSER_MOUSE_EXT_SGR : VTPARSER_MOUSE_EXT_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1015: /* urxvt mouse ext */
        p->mouse_report_ext_mode =
            on ? VTPARSER_MOUSE_EXT_URXVT : VTPARSER_MOUSE_EXT_NONE;
        if (p->cbs.set_mouse_mode) {
            p->cbs.set_mouse_mode(p, p->mouse_report_mode,
                                  p->mouse_report_ext_mode);
        }
        break;
    case 1048: /* save/restore cursor */
        if (on) SaveCursor(p);
        else    RestoreCursor(p);
        break;
    case 1049: /* alt-screen + save/restore */
        if (on) {
            SaveCursor(p);
        }
        if (p->cbs.switch_screen) {
            p->cbs.switch_screen(p, on);
        }
        p->alt_scr = on;
        if (!on) {
            RestoreCursor(p);
        }
        break;
    case 2004: /* bracketed paste */
        p->bracketed_paste = on;
        break;
    case 7727: /* MinTTY application escape mode */
        p->appli_escape_mode = on ? 1 : 0;
        break;
    case 7786: /* wheel-to-cursor */
        p->accept_wheel_to_cursor = on;
        break;
    case 8200: /* ClearThenHome */
        p->clear_then_home = on;
        break;
    case 69:   /* DECLRMM — left/right margin mode */
        p->lr_margin_mode = on;
        break;
    default:
        break;
    }
}

static void CSQ_h_Mode(VtParser *p) {  /* DECSET */
    int i;
    for (i = 1; i <= p->n_param; i++) {
        ApplyDECMode(p, p->param[i], true);
    }
}

static void CSQ_l_Mode(VtParser *p) {  /* DECRST */
    int i;
    for (i = 1; i <= p->n_param; i++) {
        ApplyDECMode(p, p->param[i], false);
    }
}

// ---------------------------------------------------------------------------
// CSI sequence handlers
// ---------------------------------------------------------------------------

static void CSInsertCharacter(VtParser *p) {   /* ICH @ */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.insert_chars) p->cbs.insert_chars(p, p->param[1]);
}

static void CSCursorUp(VtParser *p, bool affect_margin) {  /* CUU A / VPB k */
    CheckParamVal(p->param[1], 9999);
    CB_CursorUp(p, p->param[1], affect_margin);
}

static void CSCursorDown(VtParser *p, bool affect_margin) {  /* CUD B / VPR e */
    CheckParamVal(p->param[1], 9999);
    CB_CursorDown(p, p->param[1], affect_margin);
}

static void CSCursorRight(VtParser *p, bool affect_margin) {  /* CUF C / HPR a */
    CheckParamVal(p->param[1], 9999);
    CB_CursorRight(p, p->param[1], affect_margin);
}

static void CSCursorLeft(VtParser *p, bool affect_margin) {  /* CUB D / HPB j */
    CheckParamVal(p->param[1], 9999);
    CB_CursorLeft(p, p->param[1], affect_margin);
}

static void CSCursorDown1(VtParser *p) {  /* CNL E */
    if (p->cbs.cursor_move) {
        /* move to column 0 first, then down */
        int x = 0, y = 0;
        if (p->cbs.get_cursor_pos) p->cbs.get_cursor_pos(p, &x, &y);
        CB_CursorMove(p, 0, y);
    }
    CSCursorDown(p, true);
}

static void CSCursorUp1(VtParser *p) {    /* CPL F */
    if (p->cbs.get_cursor_pos) {
        int x = 0, y = 0;
        p->cbs.get_cursor_pos(p, &x, &y);
        CB_CursorMove(p, 0, y);
    }
    CSCursorUp(p, true);
}

static void CSMoveToColumnN(VtParser *p) {  /* CHA G / HPA ` */
    CheckParamVal(p->param[1], 9999);
    int new_col = p->param[1] - 1;
    if (p->cbs.cursor_move) {
        int x = 0, y = 0;
        if (p->cbs.get_cursor_pos) p->cbs.get_cursor_pos(p, &x, &y);
        CB_CursorMove(p, new_col, y);
    }
}

static void CSMoveToXY(VtParser *p) {  /* CUP H / HVP f */
    RequiredParams(p, 2);
    CheckParamVal(p->param[1], 9999);
    CheckParamVal(p->param[2], 9999);
    int new_y = p->param[1] - 1;
    int new_x = p->param[2] - 1;
    CB_CursorMove(p, new_x, new_y);
}

static void CSForwardTab(VtParser *p) {  /* CHT I */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.forward_tab) p->cbs.forward_tab(p, p->param[1]);
}

static void CSBackwardTab(VtParser *p) {  /* CBT Z */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.backward_tab) p->cbs.backward_tab(p, p->param[1]);
}

static void CSMoveToLineN(VtParser *p) {  /* VPA d */
    CheckParamVal(p->param[1], 9999);
    int new_row = p->param[1] - 1;
    if (p->cbs.get_cursor_pos) {
        int x = 0, y = 0;
        p->cbs.get_cursor_pos(p, &x, &y);
        CB_CursorMove(p, x, new_row);
    }
}

static void CSScreenErase(VtParser *p) {  /* ED J */
    if (p->cbs.erase_display) {
        p->cbs.erase_display(p, p->param[1]);
    }
    CB_UpdateScreen(p);
}

static void CSLineErase(VtParser *p) {    /* EL K */
    if (p->cbs.erase_line) {
        p->cbs.erase_line(p, p->param[1]);
    }
}

static void CSInsertLine(VtParser *p) {   /* IL L */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.insert_lines) p->cbs.insert_lines(p, p->param[1]);
}

static void CSDeleteNLines(VtParser *p) {  /* DL M */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.delete_lines) p->cbs.delete_lines(p, p->param[1]);
}

static void CSDeleteCharacter(VtParser *p) {  /* DCH P */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.delete_chars) p->cbs.delete_chars(p, p->param[1]);
}

static void CSScrollUp(VtParser *p) {    /* SU S */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.scroll_up) p->cbs.scroll_up(p, p->param[1]);
}

static void CSScrollDown(VtParser *p) {  /* SD T */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.scroll_down) p->cbs.scroll_down(p, p->param[1]);
}

static void CSEraseCharacter(VtParser *p) {  /* ECH X */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.erase_chars) p->cbs.erase_chars(p, p->param[1]);
}

static void CSRepeatCharacter(VtParser *p) {  /* REP b */
    CheckParamVal(p->param[1], 9999);
    if (p->cbs.repeat_char) p->cbs.repeat_char(p, p->param[1]);
    p->last_put_character = 0;
}

static void CSDeleteTabStop(VtParser *p) {  /* TBC g */
    if (p->cbs.clear_tab_stop) p->cbs.clear_tab_stop(p, p->param[1]);
}

static void CSSetScrollRegion(VtParser *p) {  /* DECSTBM r */
    RequiredParams(p, 2);
    CheckParamVal(p->param[1], 9999);
    CheckParamValMax(p->param[2], 9999);
    if (p->cbs.set_scroll_region) {
        p->cbs.set_scroll_region(p, p->param[1] - 1, p->param[2] - 1);
    }
    CB_CursorMove(p, 0, 0);
}

static void CSSetLRScrollRegion(VtParser *p) {  /* DECSLRM s */
    RequiredParams(p, 2);
    if (p->cbs.set_lr_scroll_region) {
        p->cbs.set_lr_scroll_region(p, p->param[1] - 1, p->param[2] - 1);
    }
}

/* Soft reset (DECSTR) */
static void SoftReset(VtParser *p) {
    p->auto_repeat_mode = true;
    p->insert_mode = false;
    p->relative_org_mode = false;
    p->appli_key_mode = false;
    p->appli_cursor_mode = false;
    p->appli_escape_mode = false;
    p->accept_wheel_to_cursor = p->cfg.translate_wheel_to_cursor;
    p->char_attr = DefaultCharAttr();
    CB_SetCharAttr(p);
    /* Reset scroll region to full screen via callback */
    if (p->cbs.set_scroll_region) p->cbs.set_scroll_region(p, 0, -1);
    if (p->cbs.set_appli_cursor) p->cbs.set_appli_cursor(p, false);
    if (p->cbs.set_appli_keypad) p->cbs.set_appli_keypad(p, false);
    SaveCursorToSlot(p, &p->saved_cursor_main);
}

static void CSExc(VtParser *p, uint8_t b) {  /* CSI ! */
    switch (b) {
    case 'p': SoftReset(p); break;
    default:  break;
    }
}

/* DECSCUSR — cursor style */
static void CSSpace(VtParser *p, uint8_t b) {  /* CSI SP */
    switch (b) {
    case 'q': /* DECSCUSR */
        if (p->cbs.cursor_set_style) {
            p->cbs.cursor_set_style(p, p->param[1], /* let cb sort blink */ 1);
        }
        break;
    default: break;
    }
}

/* DECSCPP — column width */
static void CSDouble(VtParser *p, uint8_t b) { /* CSI " */
    (void)p; (void)b;
    /* DECSCA etc — no-op for now */
}

/* Colour report (xterm) */
static void CSAster(VtParser *p, uint8_t b) { /* CSI * */
    switch (b) {
    case 'q': /* DECSACE */ break;
    default:  break;
    }
    (void)p;
}

static void CSQuote(VtParser *p, uint8_t b) { /* CSI ' */
    (void)p; (void)b;
}

/* CSI < — DEC locator */
static void CSLT(VtParser *p, uint8_t b) {
    (void)p; (void)b;
}

/* CSI = — MinTTY */
static void CSEQ(VtParser *p, uint8_t b) {
    switch (b) {
    case 'c': {
        /* Tertiary DA */
        char buf[] = "=0c";
        SendCSIstr(p, buf, -1);
        break;
    }
    default: break;
    }
}

/* CSI > — secondary DA */
static void CSGT(VtParser *p, uint8_t b) {
    switch (b) {
    case 'c': {
        /* Secondary DA: VT220 */
        char buf[] = ">1;10;0c";
        SendCSIstr(p, buf, -1);
        break;
    }
    case 'm': /* xterm modifyOtherKeys */ break;
    case 'n': /* xterm modifyKeyboard  */ break;
    default:  break;
    }
}

/* CSI $ intermediate */
static void CSDol(VtParser *p, uint8_t b) {
    switch (b) {
    case 'p': /* DECRQM — request mode */ break;
    case 'r': /* DECCARA */ break;
    case 't': /* DECRARA */ break;
    case 'v': /* DECCRA  */ break;
    case 'x': /* DECFRA  */ break;
    case 'z': /* DECERA  */ break;
    case '{': /* DECSERA */ break;
    default:  break;
    }
    (void)p;
}

/* CSI ? $ */
static void CSQDol(VtParser *p, uint8_t b) {
    (void)p; (void)b;
}

/* DECSTBM query */
static void CSQ_i_Mode(VtParser *p);  /* forward declaration */
static void CSQuest(VtParser *p, uint8_t b) {
    switch (b) {
    case 'J': /* DECSED */ if (p->cbs.erase_display) p->cbs.erase_display(p, p->param[1]); break;
    case 'K': /* DECSEL */ if (p->cbs.erase_line)    p->cbs.erase_line(p, p->param[1]);    break;
    case 'h': CSQ_h_Mode(p); break;
    case 'i': CSQ_i_Mode(p); break;
    case 'l': CSQ_l_Mode(p); break;
    case 'n': CSQ_n_Mode(p); break;
    default:  break;
    }
}

/* xterm/dtterm window operations — CSI Ps t */
static void CSSunSequence(VtParser *p) {
    if (p->cbs.window_op) {
        RequiredParams(p, 3);
        p->cbs.window_op(p, p->param[1], p->param[2], p->param[3]);
    }
}

/* DECMC — media copy (printer, auto-print mode) */
static void CSQ_i_Mode(VtParser *p) {
    switch (p->param[1]) {
    case 1: p->auto_print_mode = true;  break;
    case 4: p->auto_print_mode = false; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Full CSI dispatch
// ---------------------------------------------------------------------------

static void ParseCS(VtParser *p, uint8_t b) {
    if (p->int_count == 0) {
        switch (p->prv) {
        case 0:
            switch (b) {
            case '@': CSInsertCharacter(p);           break;  /* ICH */
            case 'A': CSCursorUp(p, true);            break;  /* CUU */
            case 'B': CSCursorDown(p, true);          break;  /* CUD */
            case 'C': CSCursorRight(p, true);         break;  /* CUF */
            case 'D': CSCursorLeft(p, true);          break;  /* CUB */
            case 'E': CSCursorDown1(p);               break;  /* CNL */
            case 'F': CSCursorUp1(p);                 break;  /* CPL */
            case 'G': CSMoveToColumnN(p);             break;  /* CHA */
            case 'H': CSMoveToXY(p);                  break;  /* CUP */
            case 'I': CSForwardTab(p);                break;  /* CHT */
            case 'J': CSScreenErase(p);               break;  /* ED  */
            case 'K': CSLineErase(p);                 break;  /* EL  */
            case 'L': CSInsertLine(p);                break;  /* IL  */
            case 'M': CSDeleteNLines(p);              break;  /* DL  */
            case 'P': CSDeleteCharacter(p);           break;  /* DCH */
            case 'S': CSScrollUp(p);                  break;  /* SU  */
            case 'T': CSScrollDown(p);                break;  /* SD  */
            case 'X': CSEraseCharacter(p);            break;  /* ECH */
            case 'Z': CSBackwardTab(p);               break;  /* CBT */
            case '`': CSMoveToColumnN(p);             break;  /* HPA */
            case 'a': CSCursorRight(p, false);        break;  /* HPR */
            case 'b': CSRepeatCharacter(p);           break;  /* REP */
            case 'c': AnswerTerminalType(p);          break;  /* DA  */
            case 'd': CSMoveToLineN(p);               break;  /* VPA */
            case 'e': CSCursorDown(p, false);         break;  /* VPR */
            case 'f': CSMoveToXY(p);                  break;  /* HVP */
            case 'g': CSDeleteTabStop(p);             break;  /* TBC */
            case 'h': CS_h_Mode(p);                   break;  /* SM  */
            case 'j': CSCursorLeft(p, false);         break;  /* HPB */
            case 'k': CSCursorUp(p, false);           break;  /* VPB */
            case 'l': CS_l_Mode(p);                   break;  /* RM  */
            case 'm': CSSetAttr(p);                   break;  /* SGR */
            case 'n': CS_n_Mode(p);                   break;  /* DSR */
            case 'r': CSSetScrollRegion(p);           break;  /* DECSTBM */
            case 's':
                if (p->lr_margin_mode) CSSetLRScrollRegion(p);
                else                   SaveCursor(p);
                break;
            case 't': CSSunSequence(p);               break;
            case 'u': RestoreCursor(p);               break;  /* RCP */
            default:  break;
            }
            break;

        case '<': CSLT(p, b);    break;
        case '=': CSEQ(p, b);    break;
        case '>': CSGT(p, b);    break;
        case '?': CSQuest(p, b); break;
        default:  break;
        }
    } else if (p->int_count == 1) {
        switch (p->prv) {
        case 0:
            switch (p->int_char[1]) {
            case ' ': CSSpace(p, b);  break;
            case '!': CSExc(p, b);    break;
            case '"': CSDouble(p, b); break;
            case '$': CSDol(p, b);    break;
            case '*': CSAster(p, b);  break;
            case '\'': CSQuote(p, b); break;
            default:  break;
            }
            break;
        case '?':
            if (p->int_char[1] == '$') CSQDol(p, b);
            break;
        default: break;
        }
    }

    p->parse_mode = kModeFirst;
}

// ---------------------------------------------------------------------------
// ControlSequence — accumulate CSI params / dispatch
// ---------------------------------------------------------------------------

static void ControlSequence(VtParser *p, uint8_t b) {
    if (b <= US || (b >= 0x80 && b <= 0x9F)) {
        /* Control char inside CSI — pass through ParseControl */
        /* (handled by caller) */
        return;
    }

    if (b >= 0x40 && b <= 0x7E) {
        ParseCS(p, b);
        return;
    }

    /* Parameter accumulation */
    if (b >= 0x30 && b <= 0x3F) {
        if (b >= '<' && b <= '?') {
            /* Private parameter leader */
            if (p->first_param) {
                p->prv = b;
            }
        } else if (b == ';') {
            /* Parameter separator */
            if (p->n_param < VTPARSER_NPARAMS_MAX) {
                p->n_param++;
                p->param[p->n_param] = 0;
                p->n_sub_param[p->n_param] = 0;
            }
        } else if (b == ':') {
            /* Sub-parameter separator */
            int cur = p->n_param;
            if (p->n_sub_param[cur] < VTPARSER_NSPARAMS_MAX) {
                p->n_sub_param[cur]++;
                p->sub_param[cur][p->n_sub_param[cur]] =
                    p->sub_param[cur][p->n_sub_param[cur] - 1];
                /* Each new sub-param starts at 0 */
                p->sub_param[cur][p->n_sub_param[cur]] = 0;
            }
        } else {
            /* Digit */
            int cur = p->n_param;
            int dig = b - '0';
            if (p->n_sub_param[cur] == 0) {
                if (p->param[cur] < 10000) {
                    p->param[cur] = p->param[cur] * 10 + dig;
                }
            } else {
                int sp = p->n_sub_param[cur];
                if (p->sub_param[cur][sp] < 10000) {
                    p->sub_param[cur][sp] =
                        p->sub_param[cur][sp] * 10 + dig;
                }
            }
        }
        p->first_param = false;
    } else if (b >= 0x20 && b <= 0x2F) {
        /* Intermediate byte */
        if (p->int_count < VTPARSER_INTCHARS_MAX) {
            p->int_count++;
        }
        p->int_char[p->int_count] = b;
    }
    /* else: 0x7F (DEL) — ignore */
}

// ---------------------------------------------------------------------------
// ESC # — DECALN / double-height / double-width
// ---------------------------------------------------------------------------

static void ESCSharp(VtParser *p, uint8_t b) {
    switch (b) {
    case '8':  /* DECALN — fill screen with E */
        if (p->cbs.fill_with_e) p->cbs.fill_with_e(p);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// ESC SP — S7C1T / S8C1T
// ---------------------------------------------------------------------------

static void ESCSpace(VtParser *p, uint8_t b) {
    switch (b) {
    case 'F': p->accept_8bit_ctrl = false; break;
    case 'G': p->accept_8bit_ctrl = true;  break;
    default:  break;
    }
}

// ---------------------------------------------------------------------------
// DCS string accumulation / dispatch
// ---------------------------------------------------------------------------

static void DeviceControl(VtParser *p, uint8_t b) {
    if (b == ST) {
        p->parse_mode = kModeFirst;
        /* Process DCS string in osc_buf */
        /* DCS $q — DECRQSS */
        /* DCS + q — xterm key definitions */
        /* For now: no-op, just reset */
        p->osc_len = 0;
        return;
    }

    /* Accumulate string until ST */
    if (b == ESC) {
        p->osc_esc_flag = true;
        return;
    }
    if (p->osc_esc_flag) {
        p->osc_esc_flag = false;
        if (b == '\\') {
            /* ST received */
            p->parse_mode = kModeFirst;
            p->osc_len = 0;
            return;
        }
        /* Not ST — put ESC back in buffer conceptually, continue */
        if (p->osc_len < VTPARSER_OSC_BUF_MAX) {
            p->osc_buf[p->osc_len++] = ESC;
        }
    }

    if (p->osc_len < VTPARSER_OSC_BUF_MAX) {
        p->osc_buf[p->osc_len++] = (char)b;
    }

    /* DCUserKey dispatch (P + digits + / + hexdata) */
    /* Handled when ST arrives */
}

// ---------------------------------------------------------------------------
// OSC / XS string accumulation + dispatch
// ---------------------------------------------------------------------------

static void DispatchOSC(VtParser *p) {
    /* OSC Ps ; Pt BEL/ST */
    int ps = 0;
    int i = 0;

    p->osc_buf[p->osc_len] = '\0';

    /* Parse numeric prefix */
    while (i < p->osc_len && p->osc_buf[i] >= '0' && p->osc_buf[i] <= '9') {
        ps = ps * 10 + (p->osc_buf[i] - '0');
        i++;
    }
    if (i < p->osc_len && p->osc_buf[i] == ';') {
        i++;  /* skip separator */
    }
    const char *pt = p->osc_buf + i;

    switch (ps) {
    case 0: /* icon + window title */
        if (p->cfg.accept_title_change) {
            if (p->cbs.on_icon_change)  p->cbs.on_icon_change(p, pt);
            if (p->cbs.on_title_change) p->cbs.on_title_change(p, pt);
        }
        break;
    case 1: /* icon title */
        if (p->cfg.accept_title_change) {
            if (p->cbs.on_icon_change)  p->cbs.on_icon_change(p, pt);
        }
        break;
    case 2: /* window title */
        if (p->cfg.accept_title_change) {
            if (p->cbs.on_title_change) p->cbs.on_title_change(p, pt);
        }
        break;
    case 4:  /* set colour slot */
    case 10: /* set default fg */
    case 11: /* set default bg */
    case 12: /* set cursor colour */
        /* Colour ops — pass to callbacks */
        if (p->cbs.set_color) {
            /* Minimal: just fire the callback with the text */
            /* Full XParseColor parsing belongs to a separate color module */
            p->cbs.set_color(p, ps, 0 /* colour parsed by caller */);
        }
        break;
    case 104: /* reset colour slot */
    case 110: /* reset fg */ case 111: /* reset bg */ case 112:
        if (p->cbs.reset_color) p->cbs.reset_color(p, ps);
        break;
    default:
        break;
    }
}

static void XSequence(VtParser *p, uint8_t b) {
    /* ST terminates the string */
    if (b == ST || b == BEL) {
        p->parse_mode = kModeFirst;
        DispatchOSC(p);
        p->osc_len = 0;
        return;
    }
    if (b == ESC) {
        p->osc_esc_flag = true;
        return;
    }
    if (p->osc_esc_flag) {
        p->osc_esc_flag = false;
        if (b == '\\') {
            p->parse_mode = kModeFirst;
            DispatchOSC(p);
            p->osc_len = 0;
            return;
        }
        if (p->osc_len < (int)p->cfg.osc_buffer_max) {
            p->osc_buf[p->osc_len++] = ESC;
        }
    }
    if (p->osc_len < (int)p->cfg.osc_buffer_max) {
        p->osc_buf[p->osc_len++] = (char)b;
    }
}

static void IgnoreString(VtParser *p, uint8_t b) {
    if (b == ST) {
        p->parse_mode = kModeFirst;
        return;
    }
    if (b == ESC) {
        p->osc_esc_flag = true;
        return;
    }
    if (p->osc_esc_flag) {
        p->osc_esc_flag = false;
        if (b == '\\') {
            p->parse_mode = kModeFirst;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ESC sequence dispatch
// ---------------------------------------------------------------------------

static void ParseEscape(VtParser *p, uint8_t b) {
    if (p->int_count == 0) {
        switch (b) {
        case '7': SaveCursor(p);                     break;  /* DECSC */
        case '8': RestoreCursor(p);                  break;  /* DECRC */
        case '=': p->appli_key_mode = true;          break;  /* DECKPAM */
        case '>': p->appli_key_mode = false;         break;  /* DECKPNM */
        case 'D': CB_LineFeed(p);                    break;  /* IND */
        case 'E': CB_CursorMove(p, 0, -1);           /* NEL — move to col 0 */
                  CB_LineFeed(p);                    break;
        case 'H': CB_SetTabStop(p);                  break;  /* HTS */
        case 'M': CB_ReverseIndex(p);                break;  /* RI */
        case 'P':  /* DCS */
            ClearParams(p);
            p->esc_flag = false;
            p->osc_len = 0;
            p->osc_esc_flag = false;
            p->parse_mode = kModeDCS;
            return;
        case 'X': /* SOS */
        case '^': /* PM  */
        case '_': /* APC */
            p->esc_flag = false;
            p->parse_mode = kModeIgnore;
            return;
        case 'Z': AnswerTerminalType(p);             break;  /* DECID */
        case '[':  /* CSI */
            ClearParams(p);
            p->first_param = true;
            p->parse_mode = kModeCSI;
            return;
        case '\\': /* ST — only relevant when we're not in a string */ break;
        case ']':  /* OSC */
            p->osc_len = 0;
            p->osc_esc_flag = false;
            p->parse_mode = kModeXS;
            return;
        case 'c':  /* RIS — hard reset */
            if (p->cbs.hide_status_line) p->cbs.hide_status_line(p);
            VtParserReset(p);
            if (p->cbs.on_title_change) p->cbs.on_title_change(p, "");
            break;
        case 'g': RingBell(p, VTPARSER_BEEP_VISUAL);  break;
        default: break;
        }
    } else if (p->int_count == 1) {
        switch (p->int_char[1]) {
        case ' ': ESCSpace(p, b);  break;
        case '#': ESCSharp(p, b);  break;
        /* charset designation — stub, full impl is Phase 2 */
        case '(': case ')': case '*': case '+':
        case '$': /* DBCS select */
        case '%':
        default: break;
        }
    } else if (p->int_count == 2) {
        /* Two intermediate chars — charset handling Phase 2 */
    }

    p->parse_mode = kModeFirst;
}

// ---------------------------------------------------------------------------
// EscapeSequence — called in kModeESC
// ---------------------------------------------------------------------------

static void EscapeSequence(VtParser *p, uint8_t b) {
    if (b <= US) {
        /* Control char inside ESC sequence — process it */
        /* (recursion-safe: we call ParseControlVT below) */
    } else if (b >= 0x20 && b <= 0x2F) {
        /* Intermediate byte */
        if (p->int_count < VTPARSER_INTCHARS_MAX) {
            p->int_count++;
        }
        p->int_char[p->int_count] = b;
    } else if (b >= 0x30 && b <= 0x7E) {
        ParseEscape(p, b);
    } else if (b >= 0x80 && b <= 0x9F) {
        /* C1 char inside ESC sequence */
    } else if (b >= 0xA0) {
        p->parse_mode = kModeFirst;
    }
    p->just_after_esc = false;
}

// ---------------------------------------------------------------------------
// ParseControlVT — C0 / C1 control character handling in VT mode
// ---------------------------------------------------------------------------

static void ParseControlVT(VtParser *p, uint8_t b) {
    if (b >= 0x80) {
        /* C1 range */
        if (!Accept8BitCtrl(p)) {
            CB_PutChar(p, b);  /* display as character in VT100 mode */
            return;
        }
        if (p->vt_level < 2) {
            b = b & 0x7F;  /* strip high bit — treat as C0 in VT100 */
        }
    }

    switch (b) {
    case ENQ:
        WriteOutput(p, p->cfg.answerback,
                    (size_t)p->cfg.answerback_len);
        break;
    case BEL:
        if (p->cfg.beep_mode != VTPARSER_BEEP_OFF) {
            RingBell(p, p->cfg.beep_mode);
        }
        break;
    case BS:
        CB_BackSpace(p);
        break;
    case HT:
        if (p->cbs.forward_tab) p->cbs.forward_tab(p, 1);
        break;
    case LF:
    case VT:
        CB_LineFeed(p);
        if (p->lf_mode) CB_CarriageReturn(p);
        break;
    case FF:
        if (p->cfg.auto_win_switch && p->just_after_esc) {
            if (p->cbs.insert_byte) {
                p->cbs.insert_byte(p, b);
                p->cbs.insert_byte(p, ESC);
            }
            p->change_emu = 1;  /* VTPARSER_EMU_TEK */
        } else {
            CB_LineFeed(p);
            if (p->lf_mode) CB_CarriageReturn(p);
        }
        break;
    case CR:
        CB_CarriageReturn(p);
        if (p->lf_mode) CB_LineFeed(p);
        break;
    case CAN:
        p->parse_mode = kModeFirst;
        break;
    case SUB:
        p->parse_mode = kModeFirst;
        break;
    case ESC:
        p->int_count = 0;
        p->just_after_esc = true;
        p->parse_mode = kModeESC;
        break;
    case FS: case GS: case RS: case US:
        if (p->cfg.auto_win_switch) {
            if (p->cbs.insert_byte) p->cbs.insert_byte(p, b);
            p->change_emu = 1;
        }
        break;
    /* C1 */
    case IND:
        CB_LineFeed(p);
        break;
    case NEL:
        CB_LineFeed(p);
        CB_CarriageReturn(p);
        break;
    case HTS:
        CB_SetTabStop(p);
        break;
    case RI:
        CB_ReverseIndex(p);
        break;
    case DCS:
        ClearParams(p);
        p->esc_flag = false;
        p->osc_len = 0;
        p->osc_esc_flag = false;
        p->parse_mode = kModeDCS;
        break;
    case SOS:
        p->esc_flag = false;
        p->parse_mode = kModeIgnore;
        break;
    case CSI:
        ClearParams(p);
        p->first_param = true;
        p->parse_mode = kModeCSI;
        break;
    case OSC:
        p->osc_len = 0;
        p->osc_esc_flag = false;
        p->parse_mode = kModeXS;
        break;
    case PM:
    case APC:
        p->esc_flag = false;
        p->parse_mode = kModeIgnore;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// ParseFirst — normal character input
// ---------------------------------------------------------------------------

static void ParseFirst(VtParser *p, uint8_t b) {
    if (b <= US || (b >= 0x80 && b <= 0x9F && Accept8BitCtrl(p))) {
        ParseControlVT(p, b);
        return;
    }
    /* Printable character or high byte */
    CB_PutChar(p, (uint32_t)b);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

VtParser *VtParserCreate(const VtParserConfig *config,
                         const VtParserCallbacks *cbs,
                         void *user_data) {
    VtParser *p = (VtParser *)calloc(1, sizeof(VtParser));
    if (!p) return NULL;

    if (config) {
        p->cfg = *config;
    } else {
        /* Sensible defaults */
        p->cfg.terminal_id    = VTPARSER_ID_VT220;
        p->cfg.accept_8bit_ctrl = true;
        p->cfg.beep_mode      = VTPARSER_BEEP_ON;
        p->cfg.beep_suppress_ms       = 1000;
        p->cfg.beep_over_used_time_ms = 5000;
        p->cfg.beep_over_used_count   = 5;
        p->cfg.beep_visual_wait_ms    = 50;
        p->cfg.osc_buffer_max = VTPARSER_OSC_BUF_MAX;
        p->cfg.accept_title_change = true;
        p->cfg.translate_wheel_to_cursor = false;
    }

    if (cbs) {
        p->cbs = *cbs;
    }

    p->user_data = user_data;

    VtParserReset(p);
    return p;
}

void VtParserReset(VtParser *p) {
    assert(p);

    p->parse_mode     = kModeFirst;
    p->esc_flag       = false;
    p->just_after_esc = false;
    p->change_emu     = 0;
    p->osc_len        = 0;
    p->osc_esc_flag   = false;

    ClearParams(p);

    /* Terminal modes — VT defaults */
    p->insert_mode           = false;
    p->lf_mode               = (p->cfg.cr_send == VTPARSER_CRSEND_CRLF);
    p->auto_wrap_mode        = true;
    p->clear_then_home       = false;
    p->relative_org_mode     = false;
    p->focus_report_mode     = false;
    p->alt_scr               = false;
    p->lr_margin_mode        = false;
    p->rectangle_mode        = false;
    p->bracketed_paste       = false;
    p->accept_wheel_to_cursor = p->cfg.translate_wheel_to_cursor;
    p->auto_repeat_mode      = true;
    p->accept_8bit_ctrl      = p->cfg.accept_8bit_ctrl;
    p->mouse_report_mode     = VTPARSER_MOUSE_NONE;
    p->mouse_report_ext_mode = VTPARSER_MOUSE_EXT_NONE;
    p->last_x = p->last_y   = 0;
    p->button_stat           = 0;
    p->printer_mode          = false;
    p->auto_print_mode       = false;
    p->print_ex              = true;
    p->appli_key_mode        = false;
    p->appli_cursor_mode     = false;
    p->appli_escape_mode     = 0;
    p->last_put_character    = 0;
    p->prev_character        = (uint8_t)-1;
    p->prev_cr_lf_generated_crlf = false;

    /* VT level */
    switch (p->cfg.terminal_id) {
    case VTPARSER_ID_VT100: p->vt_level = 1; break;
    case VTPARSER_ID_VT220:
    case VTPARSER_ID_VT382:
    case VTPARSER_ID_VT420:
    case VTPARSER_ID_VT520: p->vt_level = 2; break;
    default:                p->vt_level = 1; break;
    }

    p->char_attr = DefaultCharAttr();

    /* Saved cursor defaults */
    memset(&p->saved_cursor_main,   0, sizeof(VtSavedCursor));
    memset(&p->saved_cursor_status, 0, sizeof(VtSavedCursor));
    memset(&p->saved_cursor_alt,    0, sizeof(VtSavedCursor));
    p->saved_cursor_main.attr   = DefaultCharAttr();
    p->saved_cursor_status.attr = DefaultCharAttr();
    p->saved_cursor_alt.attr    = DefaultCharAttr();
    p->saved_cursor_main.auto_wrap_mode   = true;
    p->saved_cursor_status.auto_wrap_mode = true;
    p->saved_cursor_alt.auto_wrap_mode    = true;

    /* Beep rate-limit init */
    uint64_t now = MonoMs();
    p->beep_start_ms    = now - p->cfg.beep_over_used_time_ms;
    p->beep_suppress_ms = now - p->cfg.beep_suppress_ms;
    p->beep_over_used_count = p->cfg.beep_over_used_count;
}

void VtParserDestroy(VtParser *p) {
    if (p) {
        free(p);
    }
}

int VtParserInput(VtParser *p, const uint8_t *data, size_t len) {
    assert(p);
    assert(data || len == 0);

    p->change_emu = 0;

    for (size_t i = 0; i < len && p->change_emu == 0; i++) {
        uint8_t b = data[i];

        switch (p->parse_mode) {
        case kModeFirst:
            ParseFirst(p, b);
            break;
        case kModeESC:
            /* Control chars are processed even inside ESC */
            if (b <= US) {
                ParseControlVT(p, b);
            } else {
                EscapeSequence(p, b);
            }
            break;
        case kModeCSI:
            if (b <= US || (b >= 0x80 && b <= 0x9F)) {
                ParseControlVT(p, b);
            } else {
                ControlSequence(p, b);
            }
            break;
        case kModeDCS:
            DeviceControl(p, b);
            break;
        case kModeXS:
            XSequence(p, b);
            break;
        case kModeSOS:
        case kModeIgnore:
            IgnoreString(p, b);
            break;
        case kModeDLE:
        case kModeCAN:
            /* Auto B-Plus / ZMODEM detection — not supported headless */
            p->parse_mode = kModeFirst;
            break;
        default:
            p->parse_mode = kModeFirst;
            break;
        }
    }

    return p->change_emu;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

void *VtParserGetUserData(const VtParser *p) {
    return p ? p->user_data : NULL;
}

VtCharAttr VtParserGetCharAttr(const VtParser *p) {
    assert(p);
    return p->char_attr;
}

void VtParserSetCharAttr(VtParser *p, const VtCharAttr *attr) {
    assert(p && attr);
    p->char_attr = *attr;
    CB_SetCharAttr(p);
}

bool VtParserGetInsertMode(const VtParser *p) {
    return p ? p->insert_mode : false;
}

void VtParserSetInsertMode(VtParser *p, bool on) {
    if (p) p->insert_mode = on;
}

bool VtParserGetAutoWrapMode(const VtParser *p) {
    return p ? p->auto_wrap_mode : true;
}

void VtParserSetAutoWrapMode(VtParser *p, bool on) {
    if (p) p->auto_wrap_mode = on;
}

bool VtParserGetBracketedPaste(const VtParser *p) {
    return p ? p->bracketed_paste : false;
}

bool VtParserGetWheelToCursor(const VtParser *p) {
    return p ? p->accept_wheel_to_cursor : false;
}

int VtParserGetMouseMode(const VtParser *p) {
    return p ? p->mouse_report_mode : VTPARSER_MOUSE_NONE;
}

bool VtParserGetFocusReportMode(const VtParser *p) {
    return p ? p->focus_report_mode : false;
}

void VtParserFocusReport(VtParser *p, bool focus) {
    if (!p || !p->focus_report_mode) return;
    if (focus) {
        WriteStr(p, "\033[I");
    } else {
        WriteStr(p, "\033[O");
    }
}

bool VtParserMouseReport(VtParser *p, int event, int button,
                         int xpos, int ypos) {
    (void)event; (void)button; (void)xpos; (void)ypos;
    if (!p) return false;
    if (p->mouse_report_mode == VTPARSER_MOUSE_NONE) return false;
    /* Full mouse encoding belongs to a separate mouse module — Phase 2 */
    return true;
}

void VtParserPasteString(VtParser *p, const char *str, size_t len) {
    if (!p || !str) return;
    if (p->bracketed_paste) {
        WriteStr(p, "\033[200~");
        WriteOutput(p, (const uint8_t *)str, len);
        WriteStr(p, "\033[201~");
    } else {
        WriteOutput(p, (const uint8_t *)str, len);
    }
}
