// 2025-01-28 10:00 v0.1.0
// vtparse.h — Headless VT100/VT220/xterm state-machine parser
// Ported from TeraTerm 5 vtterm.c — Win32-free, C99, SoC-clean.
// All external effects are dispatched through VtParserCallbacks.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2025 ttcore-port contributors — BSD 3-Clause (see vtterm.c)

#ifndef VTPARSE_H_
#define VTPARSE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define VTPARSER_NPARAMS_MAX    16
#define VTPARSER_NSPARAMS_MAX   16
#define VTPARSER_INTCHARS_MAX    5
#define VTPARSER_FUNC_KEY_STR_MAX 256
#define VTPARSER_OSC_BUF_MAX    4096

// Terminal IDs (TerminalID / VTlevel)
#define VTPARSER_ID_DUMB   0
#define VTPARSER_ID_VT100  1
#define VTPARSER_ID_VT220  2
#define VTPARSER_ID_VT382  3
#define VTPARSER_ID_VT420  4
#define VTPARSER_ID_VT520  5

// Beep types (maps to ts.Beep)
#define VTPARSER_BEEP_OFF     0
#define VTPARSER_BEEP_ON      1
#define VTPARSER_BEEP_VISUAL  2

// Mouse tracking modes
#define VTPARSER_MOUSE_NONE      0
#define VTPARSER_MOUSE_X10       1
#define VTPARSER_MOUSE_NORMAL    2
#define VTPARSER_MOUSE_BTN_EVT   3
#define VTPARSER_MOUSE_ANY_EVT   4
#define VTPARSER_MOUSE_FOCUS     5

// Mouse extension modes
#define VTPARSER_MOUSE_EXT_NONE  0
#define VTPARSER_MOUSE_EXT_UTF8  1
#define VTPARSER_MOUSE_EXT_SGR   2
#define VTPARSER_MOUSE_EXT_URXVT 3

// Erase modes (ED / EL)
#define VTPARSER_ERASE_TO_END    0
#define VTPARSER_ERASE_TO_BEGIN  1
#define VTPARSER_ERASE_ALL       2
#define VTPARSER_ERASE_SAVED     3

// Cursor shapes (DECSCUSR)
#define VTPARSER_CURSOR_BLOCK         0
#define VTPARSER_CURSOR_BLOCK_BLINK   1
#define VTPARSER_CURSOR_BLOCK_STEADY  2
#define VTPARSER_CURSOR_UNDERLINE_BLINK  3
#define VTPARSER_CURSOR_UNDERLINE_STEADY 4
#define VTPARSER_CURSOR_BAR_BLINK    5
#define VTPARSER_CURSOR_BAR_STEADY   6

// CR/LF send mode
#define VTPARSER_CRSEND_CR    0
#define VTPARSER_CRSEND_LF    1
#define VTPARSER_CRSEND_CRLF  2

// ---------------------------------------------------------------------------
// Character attribute
// ---------------------------------------------------------------------------

typedef struct {
    bool bold;
    bool underline;
    bool blink;
    bool reverse;
    bool invisible;
    bool strikethrough;
    bool double_underline;
    bool overline;
    uint8_t underline_style;   // 0=off 1=single 2=double 3=curly 4=dotted 5=dashed
    uint32_t fg;               // RGB24; 0x01000000 = default
    uint32_t bg;               // RGB24; 0x01000000 = default
    uint32_t underline_color;  // RGB24; 0x01000000 = default
} VtCharAttr;

#define VTPARSER_COLOR_DEFAULT  0x01000000u

// ---------------------------------------------------------------------------
// Terminal configuration (replaces relevant ts.* fields)
// ---------------------------------------------------------------------------

typedef struct {
    int  terminal_id;          // VTPARSER_ID_*
    bool accept_8bit_ctrl;     // Accept C1 8-bit control codes
    int  cr_send;              // VTPARSER_CRSEND_*
    int  cr_receive;           // IdCR/IdLF/IdCRLF
    bool auto_win_switch;      // Enter TEK mode on FF/FS/GS/RS/US
    bool translate_wheel_to_cursor;
    bool disable_app_keypad;
    bool disable_app_cursor;
    bool disable_mouse_tracking_by_ctrl;
    bool disable_wheel_to_cursor_by_ctrl;
    int  beep_mode;            // VTPARSER_BEEP_*
    uint32_t beep_suppress_ms; // Beep suppression window (ms)
    uint32_t beep_over_used_time_ms;
    uint32_t beep_over_used_count;
    uint32_t beep_visual_wait_ms;
    uint8_t  answerback[32];
    int      answerback_len;
    uint32_t osc_buffer_max;   // max OSC string length
    bool     accept_title_change;
    bool     scroll_window_clear_screen;
    bool     enabled_continued_line_copy;
    bool     vt_compat_tab;
} VtParserConfig;

// ---------------------------------------------------------------------------
// Callbacks — every external effect goes through here
// ---------------------------------------------------------------------------

// Forward declaration so callbacks can take VtParser*
typedef struct VtParser VtParser;

typedef struct {
    // --- IO ---
    // Send bytes to the remote host
    void (*write_output)(VtParser *p, const uint8_t *data, size_t len);
    // Push one byte back to the front of the input queue
    void (*insert_byte)(VtParser *p, uint8_t b);

    // --- Screen: character output ---
    void (*put_char)(VtParser *p, uint32_t codepoint, const VtCharAttr *attr);

    // --- Screen: cursor ---
    void (*cursor_move)(VtParser *p, int x, int y);        // absolute
    void (*cursor_up)(VtParser *p, int n, bool affect_margin);
    void (*cursor_down)(VtParser *p, int n, bool affect_margin);
    void (*cursor_right)(VtParser *p, int n, bool affect_margin);
    void (*cursor_left)(VtParser *p, int n, bool affect_margin);
    void (*cursor_save)(VtParser *p);
    void (*cursor_restore)(VtParser *p);
    void (*cursor_set_style)(VtParser *p, int style, bool blink);

    // --- Screen: erase ---
    void (*erase_line)(VtParser *p, int mode);             // EL
    void (*erase_display)(VtParser *p, int mode);          // ED
    void (*erase_chars)(VtParser *p, int n);               // ECH
    void (*fill_with_e)(VtParser *p);                      // DECALN

    // --- Screen: insert/delete ---
    void (*insert_lines)(VtParser *p, int n);              // IL
    void (*delete_lines)(VtParser *p, int n);              // DL
    void (*insert_chars)(VtParser *p, int n);              // ICH
    void (*delete_chars)(VtParser *p, int n);              // DCH
    void (*repeat_char)(VtParser *p, int n);               // REP

    // --- Screen: scroll ---
    void (*scroll_up)(VtParser *p, int n);                 // SU
    void (*scroll_down)(VtParser *p, int n);               // SD
    void (*set_scroll_region)(VtParser *p, int top, int bot);  // DECSTBM
    void (*set_lr_scroll_region)(VtParser *p, int left, int right); // DECSLRM

    // --- Screen: tab stops ---
    void (*set_tab_stop)(VtParser *p);                     // HTS
    void (*clear_tab_stop)(VtParser *p, int mode);         // TBC
    void (*forward_tab)(VtParser *p, int n);               // CHT
    void (*backward_tab)(VtParser *p, int n);              // CBT

    // --- Screen: attributes ---
    void (*set_char_attr)(VtParser *p, const VtCharAttr *attr);  // SGR applied

    // --- Screen: misc ---
    void (*reverse_index)(VtParser *p);                    // RI
    void (*line_feed)(VtParser *p);                        // LF/IND/NEL
    void (*carriage_return)(VtParser *p);                  // CR
    void (*back_space)(VtParser *p);                       // BS
    void (*update_screen)(VtParser *p);                    // flush / UpdateWindow

    // --- Color ---
    void (*set_color)(VtParser *p, int slot, uint32_t rgb);
    void (*reset_color)(VtParser *p, int slot);
    // Query color: reply written via write_output
    void (*query_color)(VtParser *p, int slot);

    // --- Terminal state ---
    void (*on_bell)(VtParser *p, int type);
    void (*on_title_change)(VtParser *p, const char *title);
    void (*on_icon_change)(VtParser *p, const char *name);
    void (*on_resize)(VtParser *p, int cols, int rows);
    void (*on_terminal_id_change)(VtParser *p, int new_id);

    // Alternate screen switch
    void (*switch_screen)(VtParser *p, bool alt);

    // Keypad / cursor key mode changes
    void (*set_appli_keypad)(VtParser *p, bool on);
    void (*set_appli_cursor)(VtParser *p, bool on);
    void (*define_user_key)(VtParser *p, int id,
                            const uint8_t *str, int len);

    // Mouse tracking mode change
    void (*set_mouse_mode)(VtParser *p, int mode, int ext_mode);

    // Status line (DECSSDT / DECSASD)
    void (*hide_status_line)(VtParser *p);
    void (*show_status_line)(VtParser *p, int type);

    // Enter TEK mode (AutoWinSwitch)
    void (*enter_tek_mode)(VtParser *p);

    // DSR response helper: caller provides cols/rows/cursor
    void (*get_cursor_pos)(VtParser *p, int *x, int *y);
    void (*get_screen_size)(VtParser *p, int *cols, int *rows);

    // Log output (plaintext log via filesys)
    void (*log_byte)(VtParser *p, uint8_t b);
    void (*log_char32)(VtParser *p, uint32_t c);

    // Window manipulation (xterm CSI t)
    void (*window_op)(VtParser *p, int op, int p1, int p2);
} VtParserCallbacks;

// ---------------------------------------------------------------------------
// Saved cursor state (DECSC/DECRC, SCOSC/SCORC)
// ---------------------------------------------------------------------------

typedef struct {
    int     cursor_x;
    int     cursor_y;
    VtCharAttr attr;
    bool    auto_wrap_mode;
    bool    relative_org_mode;
} VtSavedCursor;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Create a parser. config and cbs are copied internally.
// user_data is passed as-is to all callbacks.
// Returns NULL on allocation failure.
VtParser *VtParserCreate(const VtParserConfig *config,
                         const VtParserCallbacks *cbs,
                         void *user_data);

// Full terminal reset (equivalent to ResetTerminal)
void VtParserReset(VtParser *p);

// Free all resources
void VtParserDestroy(VtParser *p);

// Feed raw bytes from the remote host.
// Returns 0 normally, VTPARSER_EMU_TEK if caller must switch to TEK mode.
#define VTPARSER_EMU_TEK  1
int VtParserInput(VtParser *p, const uint8_t *data, size_t len);

// Convenience: feed a single byte
static inline int VtParserInputByte(VtParser *p, uint8_t b) {
    return VtParserInput(p, &b, 1);
}

// Config/state accessors
void      *VtParserGetUserData(const VtParser *p);
VtCharAttr VtParserGetCharAttr(const VtParser *p);
void       VtParserSetCharAttr(VtParser *p, const VtCharAttr *attr);
bool       VtParserGetInsertMode(const VtParser *p);
void       VtParserSetInsertMode(VtParser *p, bool on);
bool       VtParserGetAutoWrapMode(const VtParser *p);
void       VtParserSetAutoWrapMode(VtParser *p, bool on);
bool       VtParserGetBracketedPaste(const VtParser *p);
bool       VtParserGetWheelToCursor(const VtParser *p);
int        VtParserGetMouseMode(const VtParser *p);
bool       VtParserGetFocusReportMode(const VtParser *p);

// Called when the terminal window gets/loses focus
void VtParserFocusReport(VtParser *p, bool focus);

// Called on mouse events; returns true if the event was consumed
bool VtParserMouseReport(VtParser *p, int event, int button,
                         int xpos, int ypos);

// Paste helpers (bracketed-paste)
void VtParserPasteString(VtParser *p, const char *str, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // VTPARSE_H_
